#include <GLFW/glfw3.h>

#include <cstdint>
#include <vector>

#include <hrl/hrl.h>
#include "example.h"

static void FramebufferSizeCallback(GLFWwindow*, int width, int height)
{
    HRL_WindowResizeCallback(width, height);
}

int main()
{
    constexpr int windowWidth = 1280;
    constexpr int windowHeight = 720;

    if (!glfwInit())
        return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(windowWidth, windowHeight, "HRL 2D Voxels", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    HRL_Init(HRL_OPENGL_33);
    HRL_InitContext(windowWidth, windowHeight, reinterpret_cast<void*>(glfwGetProcAddress));
    glfwSetFramebufferSizeCallback(window, FramebufferSizeCallback);

    const HRL_id scene = HRL_CreateScene(HRL_TRUE);
    const HRL_id camera = HRL_CreateCamera(scene, HRL_PERSPECTIVE);
    HRL_SetCameraPerspectiveFov(camera, 20.f);
    const HRL_id viewport = HRL_CreateViewport(scene, camera, 0.f, 0.f, 1.f, 1.f);


    HRL_id light = HRL_CreateLight(scene, HRL_SKY_LIGHT);
    HRL_SetLightIntensity(light, 0.1f);
    HRL_SetLightRotation(light, -40.f, 0, 0);


    HRL_id post_mat = HRL_CreateMaterial(HRL_DEFAULT_POST_PROCESS_SHADER);
    HRL_CreatePostProcess(viewport, post_mat, 1);
    HRL_MaterialSetFloat(post_mat, "vignetteStrength", 0.4f);
    HRL_MaterialSetFloat(post_mat, "bloomStrength", 1.f);


    HRL_SetVoxelSize(scene, 96, 64);
    HRL_SetVoxelPhysicalSize(scene, 1.f);
    HRL_SetVoxelChunkSize(scene, 16);

    HRL_SetVoxelTypeColor(scene, 1, 0.20f, 0.75f, 0.30f, 1.f);
    HRL_SetVoxelTypeColor(scene, 2, 0.75f, 0.45f, 0.15f, 1.f);
    HRL_SetVoxelTypeColor(scene, 3, 0.25f, 0.45f, 0.90f, 1.f);
    HRL_SetVoxelTypeColor(scene, 4, 2.f, 0, 0, 1.f);
    HRL_SetVoxelTypeEmissiveColor(scene, 4, 2.f, 2.f, 2.f);

    std::vector<HRL_Voxel> voxels(96u * 64u, {0u});
    for (int y = 4; y < 60; ++y)
    {
        for (int x = 4; x < 92; ++x)
        {
            uint32_t type = 1u;
            if ((x / 8 + y / 8) % 2 == 1)
                type = 2u;
            if ((x - 48) * (x - 48) + (y - 32) * (y - 32) < 10 * 10)
                type = 3u;
            voxels[static_cast<size_t>(y) * 96u + static_cast<size_t>(x)].type = type;
        }
    }

    if (HRL_LoadVoxelWorld(scene, voxels.data(), voxels.size()) != HRL_TRUE)
    {
        HRL_DeleteScene(scene);
        HRL_Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    bool previousSpace = false;
    uint32_t editedType = 0u;


    float camX;
    float camY;
    float camZ = 200.f;
    HRL_VoxelToWorldCoordinates(scene, 10, 10, &camX, &camY);


    HRL_SetCameraOrthoVertical(camera, 36.f);
    HRL_SetCameraNearPlane(camera, 0.1f);
    HRL_SetCameraFarPlane(camera, 1000.f);
    HRL_SetCameraLocation(camera, camX, camY, camZ);
    HRL_SetCameraRotation(camera, 0.f, -90.f, 0.f);


    while (!glfwWindowShouldClose(window))
    {
        double mouseX, mouseY;
        glfwGetCursorPos(window, &mouseX, &mouseY);

        glfwPollEvents();

        const bool space = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (space && !previousSpace)
        {
            int vx, vy;
            if (HRL_GetVoxelAtScreenPosition(
                    scene,
                    static_cast<int>(mouseX),
                    static_cast<int>(mouseY),
                    &vx,
                    &vy))
            {
                HRL_SetVoxelType(scene, vx, vy, 4);
                std::cout << HRL_GetVoxelType(scene, vx, vy) << std::endl;
            }
        }
        previousSpace = space;


        if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS)
        {
            camY += 0.1f;
            HRL_SetCameraLocation(camera, camX, camY, camZ);
        }
        if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS)
        {
            camY -= 0.1f;
            HRL_SetCameraLocation(camera, camX, camY, camZ);
        }
        if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS)
        {
            camX += 0.1f;
            HRL_SetCameraLocation(camera, camX, camY, camZ);
        }
        if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS)
        {
            camX -= 0.1f;
            HRL_SetCameraLocation(camera, camX, camY, camZ);
        }

        if (glfwGetKey(window, GLFW_KEY_F1) == GLFW_PRESS)
        {
            HRL_SaveVoxelWorldAllFile(scene, "save.hrlv");
        }
        if (glfwGetKey(window, GLFW_KEY_F2) == GLFW_PRESS)
        {
            size_t save_size;
            std::string save_data = example::OpenFile("save.hrlv", &save_size);
            HRL_LoadVoxelWorldBuffer(scene, save_data.c_str(), save_size);
        }

        HRL_BeginFrame();
        HRL_EndFrame();
        glfwSwapBuffers(window);
    }

    (void)viewport;
    HRL_DeleteScene(scene);
    HRL_Shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
