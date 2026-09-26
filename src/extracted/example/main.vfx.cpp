#include "../hrl.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

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

    // ------------------------------------------------------------
    // One system containing fire + smoke emitters.
    // ------------------------------------------------------------
    HRL_id explosion = HRL_CreateVFXSystem(scene);
    HRL_SetVFXSystemPosition(explosion, 0.0f, 0.0f, 0.0f);
    HRL_SetVFXSystemScale(explosion, 1.0f, 1.0f, 1.0f);
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
