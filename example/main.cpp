// HRL single header
#include <hrl/hrl.h>
#include <hrl/hrl_gl.h>
#include <hrl/hrl_vulkan.h>

// Window
#include <iosfwd>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <fstream>

#include <iostream>
#include <string>
#include <future>
#include <iterator>
#include <cstdio>

#include "src/example.h"

#include <glm/glm.hpp>


typedef struct {
  float x, y, z;
} vec3;
// ---------------------------------------------------------------------------
// Parallel application-side resource reads. HRL always receives data + size.
// ---------------------------------------------------------------------------
static std::string ReadBinaryFile(const char* path)
{
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  return std::string((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
}

static std::future<std::string> ReadBinaryFileAsync(const char* path)
{
  return std::async(std::launch::async, [path]() {
    return ReadBinaryFile(path);
  });
}

static HRL_id QueueTextureFromFuture(std::future<std::string>& future, const char* label)
{
  std::string data = future.get();
  if (data.empty())
  {
    std::printf("Impossible de lire %s\n", label);
    return HRL_INVALID_ID;
  }

  HRL_id id = HRL_CreateTextureAsync(data.data(), data.size());
  if (id == HRL_INVALID_ID)
    std::printf("Impossible de lancer le chargement async de %s\n", label);
  return id;
}



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
float mouseSensitivity = 0.2f;

float yaw = -90.0f;   // rotation autour de Y — regarde vers -Z au démarrage
float pitch = 0.0f;  // rotation autour de X

float camX = 0.0f;
float camY = 0.0f;
float camZ = 5.0f;

double lastMouseX = 0.0;
double lastMouseY = 0.0;
bool firstMouse = true;

// Rotation de la caméra uniquement lorsque le clic droit est maintenu
bool cameraRotating = false;


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
    (float)(cosf(y) * cosf(p)),
    (float)(sinf(p)),
    (float)(sinf(y) * cosf(p))
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
  {
    camX += forward.x * velocity;
    camY += forward.y * velocity;
    camZ += forward.z * velocity;
  }

  if (glfwGetKey(win, GLFW_KEY_S) == GLFW_PRESS)
  {
    camX -= forward.x * velocity;
    camY -= forward.y * velocity;
    camZ -= forward.z * velocity;
  }

  // A / D — gauche / droite (strafe)
  if (glfwGetKey(win, GLFW_KEY_A) == GLFW_PRESS)
  {
    camX -= right.x * velocity;
    camZ -= right.z * velocity;
  }

  if (glfwGetKey(win, GLFW_KEY_D) == GLFW_PRESS)
  {
    camX += right.x * velocity;
    camZ += right.z * velocity;
  }

  // Q / E — monter / descendre (world up, indépendant du pitch)
  if (glfwGetKey(win, GLFW_KEY_Q) == GLFW_PRESS)
    camY -= velocity;

  if (glfwGetKey(win, GLFW_KEY_E) == GLFW_PRESS)
    camY += velocity;
}


void ProcessCameraRotation(GLFWwindow* win)
{
  // La caméra ne tourne que lorsque le clic droit est maintenu
  if (!cameraRotating)
  {
    firstMouse = true;
    return;
  }

  double mouseX, mouseY;
  glfwGetCursorPos(win, &mouseX, &mouseY);

  if (firstMouse)
  {
    lastMouseX = mouseX;
    lastMouseY = mouseY;
    firstMouse = false;
  }

  float offsetX =
      (float)(mouseX - lastMouseX) * mouseSensitivity;

  float offsetY =
      (float)(lastMouseY - mouseY) * mouseSensitivity; // Y inversé

  lastMouseX = mouseX;
  lastMouseY = mouseY;

  yaw += offsetX;
  pitch += offsetY;

  // clamp pitch pour éviter de regarder trop haut/bas
  if (pitch > 89.0f)
    pitch = 89.0f;

  if (pitch < -89.0f)
    pitch = -89.0f;
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


// mouse callbacks -> gizmo et UI
static void MouseMove(GLFWwindow*, double x, double y)
{
  HRL_MouseMovedCallback(
      static_cast<float>(x),
      static_cast<float>(y)
  );
}


static void MouseButton(GLFWwindow* win, int button, int action, int)
{
  // Clic droit = contrôle de la caméra
  if (button == GLFW_MOUSE_BUTTON_RIGHT)
  {
    if (action == GLFW_PRESS)
    {
      cameraRotating = true;
      firstMouse = true;

      // Capturer la souris dans la fenêtre
      glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    }
    else if (action == GLFW_RELEASE)
    {
      cameraRotating = false;
      firstMouse = true;

      // Libérer la souris
      glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }

    return;
  }

  // Clic gauche = HRL / gizmo / UI
  if (button == GLFW_MOUSE_BUTTON_LEFT)
  {
    HRL_MouseButtonCallback(
        HRL_MOUSE_BUTTON_LEFT,
        action == GLFW_PRESS
            ? HRL_MOUSE_PRESS
            : HRL_MOUSE_RELEASE
    );
  }
}


static int CreateSurface(
    void* instance_ptr,
    void* surface_out_ptr,
    void* user_data)
{
    VkInstance instance =
        reinterpret_cast<VkInstance>(instance_ptr);

    VkSurfaceKHR* surface =
        reinterpret_cast<VkSurfaceKHR*>(surface_out_ptr);

    GLFWwindow* window =
        static_cast<GLFWwindow*>(user_data);

    return static_cast<int>(
        glfwCreateWindowSurface(
            instance,
            window,
            nullptr,
            surface));
}


int main()
{

    glfwInit();
    /*
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

  GLFWwindow* win =
      glfwCreateWindow(
          1280,
          720,
          "HRL Vulkan",
          nullptr,
          nullptr
      );

    // important! : le contexte doit etre actif avant HRL_InitContext
    glfwMakeContextCurrent(win);


    uint32_t extension_count = 0;
    const char** extensions =
        glfwGetRequiredInstanceExtensions(&extension_count);

    HRL_Vulkan_SetInstanceExtensions(
        extensions,
        extension_count);

    HRL_Vulkan_SetSurfaceCallback(
        CreateSurface,
        win);

    HRL_Init(HRL_VULKAN);
    HRL_InitContext(1280, 720, nullptr);
    */


    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* win =
      glfwCreateWindow(
          1280,
          720,
          "HRL OpenGL",
          nullptr,
          nullptr
          );

    // important! : le contexte doit etre actif avant HRL_InitContext
    glfwMakeContextCurrent(win);

    HRL_Init(HRL_OPENGL_33);
    HRL_InitContext(1280, 720, (void*)glfwGetProcAddress);


  glfwSetFramebufferSizeCallback(win, framebuffer_size_callback);

  // Ne pas cacher/verrouiller la souris au démarrage.
  // Elle sera capturée uniquement avec le clic droit.

  // désactiver la v-sync
  glfwSwapInterval(0);


  glfwSetCursorPosCallback(win, MouseMove);
  glfwSetMouseButtonCallback(win, MouseButton);


  HRL_RegisterErrorCallback(ErrorCallback);


  // ---------------------------------------------------------------------------
  // Launch all resource reads in parallel.
  // ---------------------------------------------------------------------------
  auto skyDataFuture = ReadBinaryFileAsync("MorningSkyHDRI011B_12K_TONEMAPPED.jpg");
  auto modelDataFuture = ReadBinaryFileAsync("model.fbx");

  auto skeletalDataFuture = std::async(
      std::launch::async,
      []() {
        std::string data = ReadBinaryFile("skeletal.fbx");
        if (data.empty())
          data = ReadBinaryFile("../skeletal.fbx");
        return data;
      }
  );

  auto floorAlbedoFuture = ReadBinaryFileAsync("floor/Tiles086_1K-JPG_Color.jpg");
  auto floorNormalFuture = ReadBinaryFileAsync("floor/Tiles086_1K-JPG_NormalGL.jpg");
  auto floorRoughnessFuture = ReadBinaryFileAsync("floor/Tiles086_1K-JPG_Roughness.jpg");
  auto floorDisplacementFuture = ReadBinaryFileAsync("floor/Tiles086_1K-JPG_Displacement.jpg");

  // Queue every texture for HRL's async CPU decode / GPU upload.
  HRL_id skyTexture = QueueTextureFromFuture(skyDataFuture, "MorningSkyHDRI011B_12K_TONEMAPPED.jpg");
  HRL_id floorAlbedoTexture = QueueTextureFromFuture(
      floorAlbedoFuture, "floor/Tiles086_1K-JPG_Color.jpg");
  HRL_id floorNormalTexture = QueueTextureFromFuture(
      floorNormalFuture, "floor/Tiles086_1K-JPG_NormalGL.jpg");
  HRL_id floorRoughnessTexture = QueueTextureFromFuture(
      floorRoughnessFuture, "floor/Tiles086_1K-JPG_Roughness.jpg");
  HRL_id floorDisplacementTexture = QueueTextureFromFuture(
      floorDisplacementFuture, "floor/Tiles086_1K-JPG_Displacement.jpg");


  HRL_SetDebugLineThickness(3.f);
  HRL_SetAntialiasingMode(HRL_ANTIALIASING_4X);

  // Create scene, camera & viewports
  HRL_id scene = HRL_CreateScene(true);
  HRL_id camera = HRL_CreateCamera(scene, HRL_PERSPECTIVE);

  HRL_SetCameraPerspectiveFov(camera, 90.f);

  HRL_id viewport =
      HRL_CreateViewport(
          scene,
          camera,
          0.f,
          0.f,
          1.f,
          1.f
      );



  // Initialiser explicitement la caméra avant le premier rendu.
  HRL_SetCameraLocation(
      camera,
      camX,
      camY,
      camZ
  );

  HRL_SetCameraRotation(
      camera,
      pitch,
      yaw,
      0.f
  );


  std::cout
      << HRL_GetGlobalIlluminationSupportedMethods()
      << std::endl;


    HRL_id post_mat = HRL_CreateMaterial(HRL_DEFAULT_POST_PROCESS_SHADER);
    HRL_id post_process = HRL_CreatePostProcess(viewport, post_mat, 1);
    HRL_MaterialSetFloat(post_mat, "bloomStrength", 0.f);




  // ---------------------------------------------------------------------------
  // Sky sphere image + environment mapping
  // sky_equirectangular.png est une texture equirectangulaire 2:1.
  // ---------------------------------------------------------------------------

  // skyTexture was queued asynchronously above.


  // Les bytes FBX sont lus en parallèle, mais HRL ne possède pas encore d'API
  // async pour parser/créer les meshes FBX : cette partie reste synchrone.

  // ---------------------------------------------------------------------------
  // FBX -> HRL_Vertex3D -> HRL_Mesh
  // ---------------------------------------------------------------------------

  std::string modelData = modelDataFuture.get();
  size_t modelSize = modelData.size();

  if (modelSize == 0)
  {
    printf("Impossible de lire model.fbx\n");

    HRL_Shutdown();
    glfwDestroyWindow(win);
    glfwTerminate();

    return 1;
  }


  size_t vertexCount = 0;

  HRL_Vertex3D* vertices =
      HRL_GetVertex3DFromFBX(
          modelData.data(),
          modelSize,
          &vertexCount
      );

  if (!vertices)
  {
    printf("Impossible de charger model.fbx\n");

    HRL_Shutdown();
    glfwDestroyWindow(win);
    glfwTerminate();

    return 1;
  }


  //HRL_id modelMaterial =
  //    HRL_CreateMaterial(HRL_MESH_3D_SHADER);

  HRL_id modelMaterial =
      HRL_CreateMaterialFromFBX(
          modelData.c_str(),
          modelSize
      );

  HRL_MaterialSetVec3(
      modelMaterial,
      "TintColor",
      0.8f,
      0.8f,
      0.8f
  );

  HRL_MaterialSetFloat(
      modelMaterial,
      "EnvironmentStrength",
      0.45f
  );


  HRL_id model =
      HRL_CreateMesh3D(
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


  printf(
      "Static model camera distance: %.3f\n",
      HRL_GetMeshCameraDistance(model)
  );


  // Gizmo
  HRL_id gizmo =
      HRL_CreateGizmo(viewport);

  HRL_SetGizmoPosition(
      gizmo,
      0.0f,
      2.0f,
      0.0f
  );

  HRL_SetGizmoMode(
      gizmo,
      HRL_GIZMO_MODE_ROTATE |
      HRL_GIZMO_MODE_TRANSLATE |
      HRL_GIZMO_MODE_SCALE
  );

  HRL_SetGizmoSpace(
      gizmo,
      HRL_GIZMO_SPACE_WORLD
  );

  HRL_SetGizmoTranslateAxes(
      gizmo,
      HRL_GIZMO_AXIS_NONE
  );

  HRL_SetGizmoScreenSize(
      gizmo,
      100.0f
  );

  HRL_SetGizmoUseScreenSize(
      gizmo,
      HRL_TRUE
  );


  // ---------------------------------------------------------------------------
  // Optional skeletal FBX. HRL receives the FBX bytes in memory, like every
  // other resource. Put skeletal.fbx next to the executable to test it.
  // ---------------------------------------------------------------------------

  HRL_id skeletalModel = HRL_INVALID_ID;

  std::string skeletalData = skeletalDataFuture.get();
  size_t skeletalSize = skeletalData.size();


  HRL_FBXResources* resources =
      HRL_LoadFBXResources(
          skeletalData.c_str(),
          skeletalSize
      );

  printf(
      "%zu materials, %zu textures\n",
      HRL_GetFBXMaterialCount(resources),
      HRL_GetFBXTextureCount(resources)
  );


  const HRL_FBXTextureInfo* albedo =
      HRL_GetFBXMaterialTexture(
          resources,
          0,
          HRL_FBX_MATERIAL_ALBEDO
      );


  HRL_FreeFBXResources(resources);


  if (skeletalSize > 0)
  {
    HRL_SkeletalMeshData* skeletalDataHRL =
        HRL_GetSkeletalMeshFromFBX(
            skeletalData.data(),
            skeletalSize
        );

    if (skeletalDataHRL)
    {
      skeletalModel =
          HRL_CreateSkeletalMesh(
              scene,
              skeletalDataHRL
          );

      if (skeletalModel != HRL_INVALID_ID)
      {
        HRL_id skeletalMaterial =
            HRL_CreateMaterialFromFBX(
                skeletalData.c_str(),
                skeletalSize
            );

        HRL_MaterialSetVec3(
            skeletalMaterial,
            "TintColor",
            0.85f,
            0.85f,
            0.9f
        );

        HRL_MaterialSetFloat(
            skeletalMaterial,
            "EnvironmentStrength",
            0.25f
        );

        HRL_SetMeshMaterial(
            skeletalModel,
            skeletalMaterial
        );

        HRL_SetMeshLocation(
            skeletalModel,
            -3.f,
            0.f,
            0.f
        );

        HRL_SetMeshScale(
            skeletalModel,
            2.f,
            2.f,
            2.f
        );

        printf(
            "Skeletal mesh: %u bones, %u animations\n",
            HRL_GetSkeletalBoneCount(skeletalModel),
            HRL_GetSkeletalAnimationCount(skeletalModel)
        );

        printf(
            "Skeletal model camera distance: %.3f\n",
            HRL_GetMeshCameraDistance(skeletalModel)
        );


        for (
            HRL_uint i = 0;
            i < HRL_GetSkeletalAnimationCount(skeletalModel);
            ++i
        )
        {
          const HRL_SkeletalAnimation* animation =
              HRL_GetSkeletalAnimation(
                  skeletalModel,
                  i
              );

          if (animation)
          {
            printf(
                "  Animation %u: %s (%.3fs, %zu frames)\n",
                i,
                animation->name
                    ? animation->name
                    : "<unnamed>",
                animation->duration,
                animation->frameCount
            );
          }
        }


        if (HRL_GetSkeletalAnimationCount(skeletalModel) > 0)
          HRL_PlaySkeletalAnimation(
              skeletalModel,
              0
          );
      }
      else
      {
        printf(
            "Impossible de creer le skeletal mesh.\n"
        );
      }

      HRL_FreeSkeletalMeshData(
          skeletalDataHRL
      );
    }
    else
    {
      printf(
          "Impossible de charger skeletal.fbx.\n"
      );
    }
  }
  else
  {
    printf(
        "Aucun skeletal.fbx trouve : le test skeletal est desactive.\n"
    );
  }


  HRL_SetMeshMaterial(
      model,
      modelMaterial
  );

  HRL_SetMeshLocation(
      model,
      3.f,
      0.f,
      0.f
  );

  HRL_SetMeshScale(
      model,
      5.f,
      5.f,
      5.f
  );


  // Automatic LOD: generated internally from LOD 0.
  HRL_SetMeshLODLevels(
      model,
      5
  );

  HRL_SetMeshLODMode(
      model,
      HRL_LOD_SCREEN_SIZE
  );

  HRL_SetMeshLODScreenThreshold(
      model,
      0.22f
  );

  HRL_SetMeshLODScreenScale(
      model,
      0.5f
  );

  HRL_SetMeshLODHysteresis(
      model,
      0.08f
  );

  HRL_SetMeshLODAutomatic(
      model,
      HRL_TRUE
  );


  printf(
      "Generated %u LOD levels\n",
      HRL_GetMeshLODCount(model)
  );

  for (
      HRL_uint level = 0;
      level < HRL_GetMeshLODCount(model);
      ++level
  )
  {
    printf(
        "  LOD %u: %zu triangles, %zu vertices\n",
        level,
        HRL_GetMeshLODTriangleCount(
            model,
            level
        ),
        HRL_GetMeshLODVertexCount(
            model,
            level
        )
    );
  }


  // ---------------------------------------------------------------------------
  // Sol 3D : utile pour voir les ombres projetées par le modèle.
  // ---------------------------------------------------------------------------

  const HRL_Vertex3D floorVertices[4] = {
    {{-1.f, 0.f, -1.f}, {0.f, 1.f, 0.f}, {0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}},
    {{ 1.f, 0.f, -1.f}, {0.f, 1.f, 0.f}, {1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}},
    {{ 1.f, 0.f,  1.f}, {0.f, 1.f, 0.f}, {1.f, 1.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}},
    {{-1.f, 0.f,  1.f}, {0.f, 1.f, 0.f}, {0.f, 1.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}}
  };

  const HRL_uint floorIndices[6] = {
    0, 2, 1,
    0, 3, 2
  };


  HRL_id floorMaterial =
      HRL_CreateMaterial(
          HRL_MESH_3D_SHADER
      );


    // ------------------------------------------------------------
    // ALBEDO
    // ------------------------------------------------------------

    HRL_id albedoTexture = floorAlbedoTexture;

    // Wait for all queued texture decodes and GPU uploads before first use.
    HRL_WaitForAllAsyncResources();

    if (!HRL_IsTextureReady(floorAlbedoTexture) ||
        !HRL_IsTextureReady(floorNormalTexture) ||
        !HRL_IsTextureReady(floorRoughnessTexture) ||
        !HRL_IsTextureReady(floorDisplacementTexture))
    {
      std::printf("Une ou plusieurs textures du sol n'ont pas pu être chargées.\n");
      HRL_Shutdown();
      glfwDestroyWindow(win);
      glfwTerminate();
      return 1;
    }

    // Configure the sky after its asynchronous upload. Preserve the original
    // procedural-sky fallback when the image is unavailable.
    if (skyTexture != HRL_INVALID_ID && HRL_IsTextureReady(skyTexture))
    {
      HRL_SetTextureMinFilter(skyTexture, HRL_FILTER_TRILINEAR);
      HRL_SetTextureMagFilter(skyTexture, HRL_FILTER_LINEAR);
      HRL_SetSkySphereEnabled(scene, HRL_TRUE);
      HRL_SetSkySphereTexture(scene, skyTexture);
      HRL_SetEnvironmentMap(scene, skyTexture);
      HRL_SetEnvironmentMappingEnabled(scene, HRL_TRUE);
    }
    else
    {
      HRL_SetSkySphereEnabled(scene, HRL_TRUE);
      HRL_SetSkySphereColors(
          scene,
          0.06f, 0.18f, 0.55f,
          0.55f, 0.72f, 0.95f,
          0.08f, 0.10f, 0.16f
      );
    }

    HRL_MaterialSetTexture(
        floorMaterial,
        HRL_T_ALBEDO,
        albedoTexture
    );

    // ------------------------------------------------------------
    // NORMAL
    // ------------------------------------------------------------

    HRL_id normalTexture = floorNormalTexture;

    HRL_MaterialSetTexture(
        floorMaterial,
        HRL_T_NORMAL,
        normalTexture
    );

    // ------------------------------------------------------------
    // ROUGHNESS
    // ------------------------------------------------------------

    HRL_id roughnessTexture = floorRoughnessTexture;

    HRL_MaterialSetTexture(
        floorMaterial,
        HRL_T_ROUGHNESS,
        roughnessTexture
    );

    // ------------------------------------------------------------
    // SCREEN SPACE DISPLACEMENT
    // ------------------------------------------------------------

    HRL_id displacementTexture = floorDisplacementTexture;

    HRL_MaterialSetTexture(
        floorMaterial,
        HRL_MATERIAL_TEXTURE_SS_DISPLACEMENT_MAPPING,
        displacementTexture
    );

    HRL_MaterialSetBool(
        floorMaterial,
        HRL_MATERIAL_PARAM_SS_DISPLACEMENT_ENABLED,
        HRL_TRUE
        );


    HRL_MaterialSetFloat(
        floorMaterial,
        HRL_MATERIAL_PARAM_SS_DISPLACEMENT_STRENGTH,
        12.0f
    );

    HRL_MaterialSetFloat(
        floorMaterial,
        HRL_MATERIAL_PARAM_SS_DISPLACEMENT_SCALE,
        1.0f
    );

    HRL_MaterialSetFloat(
        floorMaterial,
        HRL_MATERIAL_PARAM_SS_DISPLACEMENT_OPACITY,
        1.0f
    );


  HRL_MaterialSetVec3(
      floorMaterial,
      "TintColor",
      0.32f,
      0.34f,
      0.38f
  );

  HRL_MaterialSetFloat(
      floorMaterial,
      "EnvironmentStrength",
      0.10f
  );


  HRL_id floorMesh =
      HRL_CreateMesh3D(
          scene,
          floorVertices,
          4,
          floorIndices,
          6
      );

  HRL_SetMeshMaterial(
      floorMesh,
      floorMaterial
  );

  HRL_SetMeshScale(
      floorMesh,
      30.f,
      1.f,
      30.f
  );

  HRL_SetMeshLocation(
      floorMesh,
      0.f,
      -2.0f,
      0.f
  );



    HRL_id sky =
    HRL_CreateLight(scene, HRL_SKY_LIGHT);
    HRL_SetLightIntensity(sky, 0.15f);
    HRL_SetLightColor(sky, 1.f, 0.9f, 0.8f);

    HRL_SetAmbientOcclusionEnabled(scene, HRL_TRUE);

    HRL_SetAmbientOcclusionStrength(scene, 0.65f);
    HRL_SetAmbientOcclusionRadius(scene, 1.0f);
    HRL_SetAmbientOcclusionBias(scene, 0.03f);
    HRL_SetAmbientOcclusionPower(scene, 1.4f);

  // ---------------------------------------------------------------------------
  // Point light + shadow map cubemap.
  // ---------------------------------------------------------------------------

  HRL_id light =
      HRL_CreateLight(
          scene,
          HRL_POINT_LIGHT
      );

  HRL_SetLightAttenuation(
      light,
      0.012f
  );

  HRL_SetLightIntensity(
      light,
      5.f
  );

  HRL_SetLightColor(
      light,
      1.f,
      0.9f,
      0.75f
  );

  HRL_SetLightLocation(
      light,
      5.f,
      7.f,
      5.f
  );

  HRL_SetLightRotation(
      light,
      0.f,
      0.f,
      0.f
  );

  HRL_SetLightCastShadows(
      light,
      HRL_TRUE
  );

  HRL_SetLightShadowResolution(
      light,
      1024
  );

  HRL_SetLightShadowBias(
      light,
      0.0015f
  );

  HRL_SetLightShadowStrength(
      light,
      0.35f
  );


  // ------------------------------------------------------------
  // One system containing fire + smoke emitters.
  // ------------------------------------------------------------

  HRL_id explosion =
      HRL_CreateVFXSystem(scene);

  HRL_SetVFXSystemPosition(
      explosion,
      0.0f,
      2.0f,
      -2.0f
  );

  HRL_SetVFXSystemScale(
      explosion,
      5.f,
      5.f,
      5.f
  );

  HRL_SetVFXSystemLooping(
      explosion,
      HRL_TRUE
  );

  HRL_SetVFXSystemDuration(
      explosion,
      4.0f
  );


  HRL_id fire =
      HRL_CreateVFXEmitter(
          explosion
      );

  HRL_SetVFXEmitterSpawnRate(
      fire,
      18.0f
  );

  HRL_SetVFXEmitterLifetime(
      fire,
      0.35f,
      0.9f
  );

  HRL_SetVFXEmitterSpawnShape(
      fire,
      HRL_VFX_SHAPE_CONE
  );

  HRL_SetVFXEmitterShapeRadius(
      fire,
      0.2f
  );

  HRL_SetVFXEmitterShapeSize(
      fire,
      0.6f,
      1.2f,
      0.6f
  );

  HRL_SetVFXEmitterShapeAngle(
      fire,
      20.0f
  );

  HRL_SetVFXEmitterInitialVelocity(
      fire,
      -0.6f,
      1.5f,
      -0.6f,
      0.6f,
      3.5f,
      0.6f
  );

  HRL_SetVFXGravity(
      fire,
      0.0f,
      -1.8f,
      0.0f
  );

  HRL_SetVFXEmitterParticleSize(
      fire,
      0.18f,
      0.18f
  );

  HRL_SetVFXEmitterBlendMode(
      fire,
      HRL_VFX_BLEND_ADDITIVE
  );

  HRL_SetVFXEmitterParticleSize(
      fire,
      1.0f,
      1.0f
  );


  HRL_id fireColor =
      HRL_CreateVFXColorCurve(
          fire
      );

  HRL_AddVFXColorKey(
      fireColor,
      0.0f,
      1.0f,
      1.0f,
      0.55f,
      0.95f
  );

  HRL_AddVFXColorKey(
      fireColor,
      0.35f,
      1.0f,
      0.25f,
      0.02f,
      0.8f
  );

  HRL_AddVFXColorKey(
      fireColor,
      1.0f,
      0.35f,
      0.02f,
      0.0f,
      0.0f
  );

  HRL_SetVFXEmitterColorCurve(
      fire,
      fireColor
  );


  HRL_id fireSize =
      HRL_CreateVFXFloatCurve(
          fire
      );

  HRL_AddVFXFloatKey(
      fireSize,
      0.0f,
      0.35f
  );

  HRL_AddVFXFloatKey(
      fireSize,
      0.20f,
      1.0f
  );

  HRL_AddVFXFloatKey(
      fireSize,
      1.0f,
      0.0f
  );

  HRL_SetVFXEmitterSizeCurve(
      fire,
      fireSize
  );


  HRL_id smoke =
      HRL_CreateVFXEmitter(
          explosion
      );

  HRL_SetVFXEmitterPosition(
      smoke,
      0.0f,
      0.25f,
      0.0f
  );

  HRL_SetVFXEmitterSpawnRate(
      smoke,
      16.0f
  );

  HRL_SetVFXEmitterLifetime(
      smoke,
      2.0f,
      4.0f
  );

  HRL_SetVFXEmitterSpawnShape(
      smoke,
      HRL_VFX_SHAPE_SPHERE
  );

  HRL_SetVFXEmitterShapeRadius(
      smoke,
      0.35f
  );

  HRL_SetVFXEmitterInitialVelocity(
      smoke,
      -0.3f,
      0.7f,
      -0.3f,
      0.3f,
      1.6f,
      0.3f
  );

  HRL_SetVFXGravity(
      smoke,
      0.0f,
      0.08f,
      0.0f
  );

  HRL_SetVFXNoise(
      smoke,
      0.7f,
      0.5f,
      0.9f
  );

  HRL_SetVFXEmitterParticleSize(
      smoke,
      0.35f,
      0.35f
  );


  HRL_id smokeColor =
      HRL_CreateVFXColorCurve(
          smoke
      );

  HRL_AddVFXColorKey(
      smokeColor,
      0.0f,
      0.25f,
      0.25f,
      0.25f,
      0.0f
  );

  HRL_AddVFXColorKey(
      smokeColor,
      0.08f,
      0.35f,
      0.35f,
      0.35f,
      0.55f
  );

  HRL_AddVFXColorKey(
      smokeColor,
      1.0f,
      0.06f,
      0.06f,
      0.06f,
      0.0f
  );

  HRL_SetVFXEmitterColorCurve(
      smoke,
      smokeColor
  );


  HRL_id smokeSize =
      HRL_CreateVFXFloatCurve(
          smoke
      );

  HRL_AddVFXFloatKey(
      smokeSize,
      0.0f,
      0.25f
  );

  HRL_AddVFXFloatKey(
      smokeSize,
      0.5f,
      1.0f
  );

  HRL_AddVFXFloatKey(
      smokeSize,
      1.0f,
      1.8f
  );

  HRL_SetVFXEmitterSizeCurve(
      smoke,
      smokeSize
  );


  // A manual burst can be added at an absolute system time.
  HRL_AddVFXBurst(
      fire,
      0.0f,
      80
  );

  HRL_AddVFXBurst(
      fire,
      2.0f,
      50
  );


  // Start the simulation.
  HRL_PlayVFXSystem(explosion);
  HRL_SetVFXSystemAutoUpdate(
      explosion,
      HRL_TRUE
  );


  while (!glfwWindowShouldClose(win))
  {
    CalculateDeltaTime();

    HRL_BeginFrame();


    // piloter le skeletal mesh
    float x, y, z;
    float rx, ry, rz;
    float sx, sy, sz;

    HRL_GetGizmoPosition(
        gizmo,
        &x,
        &y,
        &z
    );

    HRL_GetGizmoRotation(
        gizmo,
        &rx,
        &ry,
        &rz
    );

    HRL_GetGizmoScale(
        gizmo,
        &sx,
        &sy,
        &sz
    );

    HRL_SetMeshLocation(
        skeletalModel,
        x,
        y,
        z
    );

    HRL_SetMeshRotation(
        skeletalModel,
        rx,
        ry,
        rz
    );

    HRL_SetMeshScale(
        skeletalModel,
        sx,
        sy,
        sz
    );


    // Rotation lente du mesh FBX pour vérifier les normales, shadows et environment map.
    static float modelYaw = 0.f;

    modelYaw += 20.f * (float)dt;




    HRL_UpdateSkeletalAnimations(
        (float)dt
    );

    HRL_EndFrame();


    glfwSwapBuffers(win);
    glfwPollEvents();


    if (dt > 0.0)
    {
      glfwSetWindowTitle(
          win,
          std::to_string(1.0 / dt).c_str()
      );
    }


    // movement de la camera
    ProcessCameraMovement(win);
    ProcessCameraRotation(win);

    HRL_SetCameraLocation(
        camera,
        camX,
        camY,
        camZ
    );

    HRL_SetCameraRotation(
        camera,
        pitch,
        yaw,
        0.f
    );


    // Debug views
    if (glfwGetKey(win, GLFW_KEY_F1) == GLFW_PRESS)
      HRL_DrawSceneAsDebugMode(
          scene,
          HRL_DEBUG_VIEW_NONE
      );

    if (glfwGetKey(win, GLFW_KEY_F2) == GLFW_PRESS)
      HRL_DrawSceneAsDebugMode(
          scene,
          HRL_DEBUG_VIEW_UNLIT
      );

    if (glfwGetKey(win, GLFW_KEY_F3) == GLFW_PRESS)
      HRL_DrawSceneAsDebugMode(
          scene,
          HRL_DEBUG_VIEW_WIREFRAME
      );

    if (glfwGetKey(win, GLFW_KEY_F4) == GLFW_PRESS)
      HRL_DrawSceneAsDebugMode(
          scene,
          HRL_DEBUG_VIEW_NORMAL
      );

    if (glfwGetKey(win, GLFW_KEY_F5) == GLFW_PRESS)
      HRL_DrawSceneAsDebugMode(
          scene,
          HRL_DEBUG_VIEW_LIGHTING
      );

    if (glfwGetKey(win, GLFW_KEY_F6) == GLFW_PRESS)
      HRL_DrawSceneAsDebugMode(
          scene,
          HRL_DEBUG_VIEW_LOD
      );


    if (glfwGetKey(win, GLFW_KEY_ESCAPE) == GLFW_PRESS)
      glfwSetWindowShouldClose(
          win,
          true
      );


    if (glfwGetKey(win, GLFW_KEY_F7) == GLFW_PRESS)
      HRL_SetAntialiasingMode(
          HRL_ANTIALIASING_8X
      );


    if (glfwGetKey(win, GLFW_KEY_F8) == GLFW_PRESS)
    {
      HRL_TakeScreenshot(
          scene,
          "screenshot.png"
      );

      float proj[16];

      HRL_GetProjectionMatrix(proj);

      for (int col = 0; col < 4; col++)
      {
        printf(
            "%f %f %f %f\n",
            proj[col * 4 + 0],
            proj[col * 4 + 1],
            proj[col * 4 + 2],
            proj[col * 4 + 3]
        );
      }
    }


    if (glfwGetKey(win, GLFW_KEY_F9) == GLFW_PRESS)
    {
      HRL_DrawDebugCircle(
          scene,
          HRL_DEBUG_SOLID,
          0.f,
          20.f,
          0.f,
          5.f,
          16,
          1.f,
          0.f,
          1.f
      );

      HRL_DrawDebugSegment(
          scene,
          0.0f,
          0.0f,
          0.0f,
          10.0f,
          0.0f,
          0.0f,
          1.0f,
          0.0f,
          0.0f
      );
    }


    if (glfwGetKey(win, GLFW_KEY_F10) == GLFW_PRESS)
    {
      HRL_SetGlobalIlluminationMethod(
          scene,
          HRL_GI_DDGI
      );

      HRL_SetGlobalIlluminationEnabled(
          scene,
          HRL_TRUE
      );

      printf(
          "GI ENABLED = %d | METHOD = %d\n",
          HRL_IsGlobalIlluminationEnabled(scene),
          (int)HRL_GetGlobalIlluminationMethod(scene)
      );
    }


    if (glfwGetKey(win, GLFW_KEY_F11) == GLFW_PRESS)
    {
      HRL_SetGlobalIlluminationEnabled(
          scene,
          HRL_FALSE
      );

      printf(
          "GI ENABLED = %d | METHOD = %d\n",
          HRL_IsGlobalIlluminationEnabled(scene),
          (int)HRL_GetGlobalIlluminationMethod(scene)
      );
    }
  }


  HRL_Shutdown();

  glfwDestroyWindow(win);
  glfwTerminate();
}
