#include <hrl/hrl.h>

#include <GLFW/glfw3.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "example.h"

static void FramebufferResize(GLFWwindow*, int width, int height)
{
    HRL_WindowResizeCallback(width, height);
}

static std::vector<char> ReadBinaryFile(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

int main()
{
    if (!glfwInit())
        return -1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "HRL VFX Example", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);

    int framebufferWidth = 1280;
    int framebufferHeight = 720;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    HRL_Init(HRL_OPENGL_33);
    HRL_InitContext((HRL_uint)framebufferWidth, (HRL_uint)framebufferHeight, (void*)glfwGetProcAddress);
    glfwSetFramebufferSizeCallback(window, FramebufferResize);

    HRL_id scene = HRL_CreateScene(HRL_TRUE);
    HRL_id camera = HRL_CreateCamera(scene, HRL_PERSPECTIVE);
    HRL_SetCameraLocation(camera, 0.0f, 1.8f, -7.0f);
    HRL_SetCameraRotation(camera, -4.0f, 90.0f, 0.0f);
    HRL_SetCameraPerspectiveFov(camera, 60.0f);

    HRL_id viewport = HRL_CreateViewport(scene, camera, 0.0f, 0.0f, 1.0f, 1.0f);
    (void)viewport;



    const HRL_Vertex3D floorVertices[4] = {
        {{-1.f, 0.f, -1.f}, {0.f, 1.f, 0.f}, {0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}},
        {{ 1.f, 0.f, -1.f}, {0.f, 1.f, 0.f}, {1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}},
        {{ 1.f, 0.f,  1.f}, {0.f, 1.f, 0.f}, {1.f, 1.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}},
        {{-1.f, 0.f,  1.f}, {0.f, 1.f, 0.f}, {0.f, 1.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}}
    };
    const HRL_uint floorIndices[6] = {0, 2, 1, 0, 3, 2};

    HRL_id floorMaterial = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
    HRL_MaterialSetVec3(floorMaterial, "TintColor", 0.32f, 0.34f, 0.38f);
    HRL_MaterialSetFloat(floorMaterial, "EnvironmentStrength", 0.10f);

    HRL_id floorMesh = HRL_CreateMesh3D(scene, floorVertices, 4, floorIndices, 6);
    HRL_SetMeshMaterial(floorMesh, floorMaterial);
    HRL_SetMeshScale(floorMesh, 30.f, 1.f, 30.f);
    HRL_SetMeshLocation(floorMesh, 0.f, -2.0f, 0.f);



    HRL_id light = HRL_CreateLight(scene, HRL_POINT_LIGHT);
    HRL_SetLightAttenuation(light, 0.012f);
    HRL_SetLightIntensity(light, 15.f);
    HRL_SetLightColor(light, 1.f, 0.95f, 0.85f);
    HRL_SetLightLocation(light, 5.f, 7.f, 5.f);
    HRL_SetLightRotation(light, 0.f, 0.f, 0.f);
    HRL_SetLightCastShadows(light, HRL_TRUE);
    HRL_SetLightShadowResolution(light, 1024);
    HRL_SetLightShadowBias(light, 0.0015f);
    HRL_SetLightShadowStrength(light, 0.35f);

    size_t skySize = 0;
    std::string skyData = example::OpenFile("skydome.jpg", &skySize);
    if (skySize == 0)
        skyData = example::OpenFile("skydome.jpg", &skySize);
    HRL_id skyTexture = HRL_INVALID_ID;
    if (skySize > 0)
        skyTexture = HRL_CreateTexture(skyData.data(), skySize);
    HRL_SetTextureMinFilter(skyTexture, HRL_FILTER_TRILINEAR);
    HRL_SetTextureMagFilter(skyTexture, HRL_FILTER_LINEAR);
    HRL_SetSkySphereEnabled(scene, HRL_TRUE);
    HRL_SetSkySphereTexture(scene, skyTexture);

    HRL_SetEnvironmentMap(scene, skyTexture);
    HRL_SetEnvironmentMappingEnabled(scene, HRL_TRUE);


    // ------------------------------------------------------------
    // One system containing fire + smoke emitters.
    // ------------------------------------------------------------
    HRL_id explosion = HRL_CreateVFXSystem(scene);
    HRL_SetVFXSystemPosition(explosion, 0.0f, 0.0f, 0.0f);
    HRL_SetVFXSystemLooping(explosion, HRL_TRUE);
    HRL_SetVFXSystemDuration(explosion, 4.0f);

    HRL_id fire = HRL_CreateVFXEmitter(explosion);
    HRL_SetVFXEmitterSpawnRate(fire, 18.0f);
    HRL_SetVFXEmitterLifetime(fire, 0.35f, 0.9f);
    HRL_SetVFXEmitterSpawnShape(fire, HRL_VFX_SHAPE_CONE);
    HRL_SetVFXEmitterShapeRadius(fire, 0.2f);
    HRL_SetVFXEmitterShapeSize(fire, 0.6f, 1.2f, 0.6f);
    HRL_SetVFXEmitterShapeAngle(fire, 20.0f);
    HRL_SetVFXEmitterInitialVelocity(fire, -0.6f, 1.5f, -0.6f, 0.6f, 3.5f, 0.6f);
    HRL_SetVFXGravity(fire, 0.0f, -1.8f, 0.0f);
    HRL_SetVFXEmitterParticleSize(fire, 0.18f, 0.18f);
    HRL_SetVFXEmitterBlendMode(fire, HRL_VFX_BLEND_ADDITIVE);


    HRL_id fireColor = HRL_CreateVFXColorCurve(fire);
    HRL_AddVFXColorKey(fireColor, 0.0f, 1.0f, 1.0f, 0.55f, 0.95f);
    HRL_AddVFXColorKey(fireColor, 0.35f, 1.0f, 0.25f, 0.02f, 0.8f);
    HRL_AddVFXColorKey(fireColor, 1.0f, 0.35f, 0.02f, 0.0f, 0.0f);
    HRL_SetVFXEmitterColorCurve(fire, fireColor);

    HRL_id fireSize = HRL_CreateVFXFloatCurve(fire);
    HRL_AddVFXFloatKey(fireSize, 0.0f, 0.35f);
    HRL_AddVFXFloatKey(fireSize, 0.20f, 1.0f);
    HRL_AddVFXFloatKey(fireSize, 1.0f, 0.0f);
    HRL_SetVFXEmitterSizeCurve(fire, fireSize);

    HRL_id smoke = HRL_CreateVFXEmitter(explosion);
    HRL_SetVFXEmitterPosition(smoke, 0.0f, 0.25f, 0.0f);
    HRL_SetVFXEmitterSpawnRate(smoke, 16.0f);
    HRL_SetVFXEmitterLifetime(smoke, 2.0f, 4.0f);
    HRL_SetVFXEmitterSpawnShape(smoke, HRL_VFX_SHAPE_SPHERE);
    HRL_SetVFXEmitterShapeRadius(smoke, 0.35f);
    HRL_SetVFXEmitterInitialVelocity(smoke, -0.3f, 0.7f, -0.3f, 0.3f, 1.6f, 0.3f);
    HRL_SetVFXGravity(smoke, 0.0f, 0.08f, 0.0f);
    HRL_SetVFXNoise(smoke, 0.7f, 0.5f, 0.9f);
    HRL_SetVFXEmitterParticleSize(smoke, 0.35f, 0.35f);

    HRL_id smokeColor = HRL_CreateVFXColorCurve(smoke);
    HRL_AddVFXColorKey(smokeColor, 0.0f, 0.25f, 0.25f, 0.25f, 0.0f);
    HRL_AddVFXColorKey(smokeColor, 0.08f, 0.35f, 0.35f, 0.35f, 0.55f);
    HRL_AddVFXColorKey(smokeColor, 1.0f, 0.06f, 0.06f, 0.06f, 0.0f);
    HRL_SetVFXEmitterColorCurve(smoke, smokeColor);

    HRL_id smokeSize = HRL_CreateVFXFloatCurve(smoke);
    HRL_AddVFXFloatKey(smokeSize, 0.0f, 0.25f);
    HRL_AddVFXFloatKey(smokeSize, 0.5f, 1.0f);
    HRL_AddVFXFloatKey(smokeSize, 1.0f, 1.8f);
    HRL_SetVFXEmitterSizeCurve(smoke, smokeSize);

    // A manual burst can be added at an absolute system time.
    HRL_AddVFXBurst(fire, 0.0f, 80);
    HRL_AddVFXBurst(fire, 2.0f, 50);

    // Start the simulation.
    HRL_PlayVFXSystem(explosion);



    HRL_id skeletalModel = HRL_INVALID_ID;
  size_t skeletalSize = 0;
  std::string skeletalData = example::OpenFile("skeletal.fbx", &skeletalSize);
  if (skeletalSize == 0)
    skeletalData = example::OpenFile("../skeletal.fbx", &skeletalSize);

  HRL_FBXResources* resources =
    HRL_LoadFBXResources(skeletalData.c_str(), skeletalSize);

  printf("%zu materials, %zu textures\n",
      HRL_GetFBXMaterialCount(resources),
      HRL_GetFBXTextureCount(resources));

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
      HRL_GetSkeletalMeshFromFBX(skeletalData.data(), skeletalSize);
    if (skeletalDataHRL)
    {
      skeletalModel = HRL_CreateSkeletalMesh(scene, skeletalDataHRL);
      if (skeletalModel != HRL_INVALID_ID)
      {
        HRL_id skeletalMaterial = HRL_CreateMaterialFromFBX(skeletalData.c_str(), skeletalSize);
        HRL_MaterialSetVec3(skeletalMaterial, "TintColor", 0.85f, 0.85f, 0.9f);
        HRL_MaterialSetFloat(skeletalMaterial, "EnvironmentStrength", 0.25f);
        HRL_SetMeshMaterial(skeletalModel, skeletalMaterial);
        HRL_SetMeshLocation(skeletalModel, -3.f, 0.f, 0.f);
        HRL_SetMeshScale(skeletalModel, 2.f, 2.f, 2.f);

        printf("Skeletal mesh: %u bones, %u animations\n", HRL_GetSkeletalBoneCount(skeletalModel), HRL_GetSkeletalAnimationCount(skeletalModel));
        printf("Skeletal model camera distance: %.3f\n", HRL_GetMeshCameraDistance(skeletalModel));

        for (HRL_uint i = 0; i < HRL_GetSkeletalAnimationCount(skeletalModel); ++i)
        {
          const HRL_SkeletalAnimation* animation = HRL_GetSkeletalAnimation(skeletalModel, i);
          if (animation)
            printf("  Animation %u: %s (%.3fs, %zu frames)\n",
              i, animation->name ? animation->name : "<unnamed>",
              animation->duration, animation->frameCount);
        }
        if (HRL_GetSkeletalAnimationCount(skeletalModel) > 0)
          HRL_PlaySkeletalAnimation(skeletalModel, 0);
      }
      else
      {
        printf("Impossible de creer le skeletal mesh.\n");
      }
      HRL_FreeSkeletalMeshData(skeletalDataHRL);
    }
    else
    {
      printf("Impossible de charger skeletal.fbx.\n");
    }
  }
  else
  {
    printf("Aucun skeletal.fbx trouve : le test skeletal est desactive.\n");
  }


    // ------------------------------------------------------------
    // Render loop. HRL_BeginFrame performs automatic VFX updates.
    // ------------------------------------------------------------
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        HRL_BeginFrame();
        HRL_EndFrame();

        glfwSwapBuffers(window);
    }

    HRL_DeleteVFXSystem(explosion);
    HRL_DeleteViewport(viewport);
    HRL_DeleteCamera(camera);
    HRL_DeleteScene(scene);
    HRL_Shutdown();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
