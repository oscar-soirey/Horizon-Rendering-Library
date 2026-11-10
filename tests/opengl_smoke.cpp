// Regression test of the deprecated OpenGL 3.3 backend: the frontend is shared
// with Vulkan, so its refactors must not change what OpenGL renders.
//
// Renders a small lit scene in a hidden window, checks picking and object
// deletion, and writes hrl_test_opengl.png. Usage: hrl_test_opengl [output.png]
#include "test_common.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

int main(int argc, char** argv)
{
	const char* output = argc > 1 ? argv[1] : "hrl_test_opengl.png";
	if (!glfwInit()) { std::fprintf(stderr, "SKIP: no display for GLFW\n"); return 77; }
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	GLFWwindow* window = glfwCreateWindow(320, 200, "hrl test", nullptr, nullptr);
	if (!window) { std::fprintf(stderr, "SKIP: no OpenGL 3.3 context\n"); glfwTerminate(); return 77; }
	glfwMakeContextCurrent(window);
	int width = 0, height = 0;
	glfwGetFramebufferSize(window, &width, &height);

	HRL_RegisterErrorCallback(OnError);
	HRL_Init(HRL_OPENGL_33);
	HRL_InitContext((HRL_uint)width, (HRL_uint)height, (void*)glfwGetProcAddress);

	const HRL_id scene = HRL_CreateScene(HRL_TRUE);
	const HRL_id camera = HRL_CreateCamera(scene, HRL_PERSPECTIVE);
	HRL_SetCameraPerspectiveFov(camera, 60.f);
	HRL_SetCameraNearPlane(camera, 0.1f);
	HRL_SetCameraFarPlane(camera, 200.f);
	HRL_SetCameraLocation(camera, 0.f, 2.f, 6.f);
	HRL_SetCameraRotation(camera, -15.f, -90.f, 0.f);
	HRL_CreateViewport(scene, camera, 0.f, 0.f, 1.f, 1.f);
	HRL_SetSkySphereEnabled(scene, HRL_TRUE);

	// Several lights: their order must not depend on the storage of the frontend.
	const HRL_id sun = HRL_CreateLight(scene, HRL_DIRECTIONAL_LIGHT);
	HRL_SetLightRotation(sun, -50.f, 30.f, 0.f);
	HRL_SetLightCastShadows(sun, HRL_TRUE);
	const float colors[3][3] = {{1.f, 0.2f, 0.2f}, {0.2f, 1.f, 0.2f}, {0.2f, 0.2f, 1.f}};
	for (int i = 0; i < 3; ++i)
	{
		const HRL_id point = HRL_CreateLight(scene, HRL_POINT_LIGHT);
		HRL_SetLightLocation(point, -3.f + 3.f * (float)i, 1.5f, 2.f);
		HRL_SetLightColor(point, colors[i][0], colors[i][1], colors[i][2]);
		HRL_SetLightIntensity(point, 3.f);
	}
	const HRL_id sky = HRL_CreateLight(scene, HRL_SKY_LIGHT);
	HRL_SetLightIntensity(sky, 0.15f);

	const HRL_id checker = MakeCheckerTexture(64, 230, 200, 60);
	CHECK(HRL_IsValidTexture(checker));
	const HRL_id groundMat = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
	HRL_MaterialSetTexture(groundMat, HRL_T_ALBEDO, checker);
	const HRL_id redMat = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
	HRL_MaterialSetVec3(redMat, "TintColor", 0.9f, 0.1f, 0.1f);
	HRL_MaterialSetInt(redMat, "RoughnessUseValue", 1);
	HRL_MaterialSetFloat(redMat, "RoughnessValue", 0.3f);

	const MeshData sphere = MakeSphere(1.f, 24, 32), ground = MakeGround(30.f);
	AddMesh(scene, ground, groundMat, 0.f, 0.f, 0.f);
	const HRL_id center = AddMesh(scene, sphere, redMat, 0.f, 1.f, 0.f);
	std::vector<HRL_id> extra;
	for (int i = 0; i < 8; ++i)
		extra.push_back(AddMesh(scene, sphere, i % 2 ? groundMat : redMat, -7.f + 2.f * (float)i, 1.f, -4.f));

	Frames(3);
	HRL_EMeshType type = HRL_SPRITE;
	CHECK(HRL_GetHoveredObject(scene, width / 2, height / 2, &type) == center);

	// Deletions and re-creations between frames.
	for (size_t i = 0; i < extra.size(); i += 2) HRL_DeleteMesh(extra[i]);
	Frames(1);
	for (size_t i = 0; i < extra.size(); i += 2)
		extra[i] = AddMesh(scene, sphere, redMat, -7.f + 2.f * (float)i, 2.f, -4.f);
	CHECK(HRL_IsValidMesh(extra[0]));

	HRL_BeginFrame();
	HRL_TakeScreenshot(scene, output);
	HRL_EndFrame();

	// A second scene created and destroyed while the first one lives.
	const HRL_id other = HRL_CreateScene(HRL_FALSE);
	const HRL_id otherCamera = HRL_CreateCamera(other, HRL_PERSPECTIVE);
	HRL_CreateViewport(other, otherCamera, 0.f, 0.f, 1.f, 1.f);
	AddMesh(other, sphere, redMat, 0.f, 0.f, -4.f);
	Frames(2);
	HRL_DeleteScene(other);
	CHECK(!HRL_IsValidScene(other));
	CHECK(HRL_IsValidScene(scene));
	Frames(2);

	HRL_Shutdown();
	glfwDestroyWindow(window);
	glfwTerminate();
	std::printf("%d check failure(s), %d HRL error(s)\n", g_failures, g_errors);
	return (g_failures || g_errors) ? 1 : 0;
}
