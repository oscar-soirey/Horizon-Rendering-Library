// HRL example: physically based 3D scene with the native Vulkan backend
// (PBR materials, shadows, image based lighting, translucency, bloom).
//
//   hrl_vulkan_pbr                       opens a window
//   hrl_vulkan_pbr --screenshot out.png  renders off-screen and writes a PNG
//   options: --size W H   --rt (ray traced shadows)   --frames N (stop after N frames)
//
// Controls: right mouse button + mouse to look, W A S D to move, Q / E down / up,
// Shift to move faster, R toggles ray traced shadows, P takes a screenshot,
// Escape quits.
#include <hrl.h>
#include <hrl_vulkan.h>

#ifdef HRL_EXAMPLE_HAS_GLFW
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
const float kPi = 3.14159265358979f;
int g_errors = 0;

void OnError(HRL_EError code, HRL_ESeverity severity, const char* detail)
{
	std::fprintf(stderr, "[HRL %s] %s: %s\n", HRL_SeverityEnumToString(severity), HRL_ErrorEnumToString(code), detail ? detail : "");
	if (severity == HRL_SEVERITY_ERROR || severity == HRL_SEVERITY_FATAL) ++g_errors;
}

struct MeshData
{
	std::vector<HRL_Vertex3D> vertices;
	std::vector<HRL_uint> indices;
};

HRL_Vertex3D Vertex(float px, float py, float pz, float nx, float ny, float nz, float u, float v, float tx, float ty, float tz)
{
	HRL_Vertex3D r{};
	r.position[0] = px; r.position[1] = py; r.position[2] = pz;
	r.normal[0] = nx; r.normal[1] = ny; r.normal[2] = nz;
	r.uv[0] = u; r.uv[1] = v;
	r.tangent[0] = tx; r.tangent[1] = ty; r.tangent[2] = tz;
	// bitangent = normal x tangent
	r.bitangent[0] = ny * tz - nz * ty;
	r.bitangent[1] = nz * tx - nx * tz;
	r.bitangent[2] = nx * ty - ny * tx;
	return r;
}

MeshData MakeSphere(float radius, int rings, int sectors)
{
	MeshData m;
	for (int r = 0; r <= rings; ++r)
	{
		const float v = (float)r / (float)rings, theta = v * kPi;
		for (int s = 0; s <= sectors; ++s)
		{
			const float u = (float)s / (float)sectors, phi = u * 2.f * kPi;
			const float nx = std::sin(theta) * std::cos(phi), ny = std::cos(theta), nz = std::sin(theta) * std::sin(phi);
			m.vertices.push_back(Vertex(nx * radius, ny * radius, nz * radius, nx, ny, nz, u, 1.f - v, -std::sin(phi), 0.f, std::cos(phi)));
		}
	}
	for (int r = 0; r < rings; ++r)
		for (int s = 0; s < sectors; ++s)
		{
			const HRL_uint a = (HRL_uint)(r * (sectors + 1) + s), b = a + (HRL_uint)sectors + 1;
			// Counter-clockwise seen from outside.
			m.indices.insert(m.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
		}
	return m;
}

// Box centred on the origin, counter-clockwise faces.
MeshData MakeBox(float hx, float hy, float hz)
{
	MeshData m;
	const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	const float t[6][3] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
	for (int f = 0; f < 6; ++f)
	{
		const float bx = n[f][1] * t[f][2] - n[f][2] * t[f][1];
		const float by = n[f][2] * t[f][0] - n[f][0] * t[f][2];
		const float bz = n[f][0] * t[f][1] - n[f][1] * t[f][0];
		const HRL_uint base = (HRL_uint)m.vertices.size();
		for (int c = 0; c < 4; ++c)
		{
			const float su = (c == 1 || c == 2) ? 1.f : -1.f, sv = (c >= 2) ? 1.f : -1.f;
			const float px = (n[f][0] + t[f][0] * su + bx * sv) * hx;
			const float py = (n[f][1] + t[f][1] * su + by * sv) * hy;
			const float pz = (n[f][2] + t[f][2] * su + bz * sv) * hz;
			m.vertices.push_back(Vertex(px, py, pz, n[f][0], n[f][1], n[f][2], su * 0.5f + 0.5f, sv * 0.5f + 0.5f, t[f][0], t[f][1], t[f][2]));
		}
		m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
	}
	return m;
}

HRL_id AddMesh(HRL_id scene, const MeshData& data, HRL_id material, float x, float y, float z)
{
	const HRL_id mesh = HRL_CreateMesh3D(scene, data.vertices.data(), data.vertices.size(), data.indices.data(), data.indices.size());
	HRL_SetMeshMaterial(mesh, material);
	HRL_SetMeshLocation(mesh, x, y, z);
	return mesh;
}

HRL_id MakeMaterial(float r, float g, float b, float roughness, float metallic)
{
	const HRL_id m = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
	HRL_MaterialSetVec3(m, "TintColor", r, g, b);
	HRL_MaterialSetInt(m, "RoughnessUseValue", 1);
	HRL_MaterialSetFloat(m, "RoughnessValue", roughness);
	HRL_MaterialSetInt(m, "MetallicUseValue", 1);
	HRL_MaterialSetFloat(m, "MetallicValue", metallic);
	return m;
}

struct Options
{
	int width = 1280, height = 720;
	std::string screenshot;
	bool rayTracedShadows = false;
	int frames = 0; // 0 : until the window is closed (3 frames off-screen)
};

struct App
{
	HRL_id scene = 0, camera = 0;
	float camX = -9.f, camY = 4.5f, camZ = 9.f, pitch = -16.f, yaw = -45.f;
};

void BuildScene(App& app, const Options& opt)
{
	app.scene = HRL_CreateScene(HRL_TRUE);
	app.camera = HRL_CreateCamera(app.scene, HRL_PERSPECTIVE);
	HRL_SetCameraPerspectiveFov(app.camera, 55.f);
	HRL_SetCameraNearPlane(app.camera, 0.1f);
	HRL_SetCameraFarPlane(app.camera, 500.f);
	const HRL_id viewport = HRL_CreateViewport(app.scene, app.camera, 0.f, 0.f, 1.f, 1.f);

	// Post-processing: exposure (in stops) and bloom.
	const HRL_id post = HRL_CreateMaterial(HRL_DEFAULT_POST_PROCESS_SHADER);
	HRL_MaterialSetFloat(post, "exposure", 0.f);
	HRL_MaterialSetFloat(post, "bloomStrength", 1.f);
	HRL_CreatePostProcess(viewport, post, 0);

	// Sky: procedural gradient, also used as the image based lighting source.
	HRL_SetSkySphereEnabled(app.scene, HRL_TRUE);
	HRL_SetSkySphereColors(app.scene, 0.05f, 0.16f, 0.55f, 0.36f, 0.50f, 0.78f, 0.12f, 0.11f, 0.10f);
	HRL_SetEnvironmentMappingEnabled(app.scene, HRL_TRUE);

	// Sun.
	const HRL_id sun = HRL_CreateLight(app.scene, HRL_DIRECTIONAL_LIGHT);
	HRL_SetLightRotation(sun, -48.f, 35.f, 0.f);
	HRL_SetLightColor(sun, 1.0f, 0.95f, 0.86f);
	HRL_SetLightIntensity(sun, 5.5f);
	HRL_SetLightCastShadows(sun, HRL_TRUE);
	HRL_SetLightShadowResolution(sun, 2048);
	// Warm spot light with its own shadow map.
	const HRL_id spot = HRL_CreateLight(app.scene, HRL_SPOT_LIGHT);
	HRL_SetLightLocation(spot, 6.f, 6.f, -5.f);
	HRL_SetLightRotation(spot, -50.f, 140.f, 0.f);
	HRL_SetLightColor(spot, 1.0f, 0.55f, 0.25f);
	HRL_SetLightIntensity(spot, 60.f);
	HRL_SetLightAttenuation(spot, 0.03f);
	HRL_SetSpotLightInnerCutoff(spot, 18.f);
	HRL_SetSpotLightOuterCutoff(spot, 28.f);
	HRL_SetLightCastShadows(spot, HRL_TRUE);
	// Cool point light (shadowed only with ray traced shadows).
	const HRL_id point = HRL_CreateLight(app.scene, HRL_POINT_LIGHT);
	HRL_SetLightLocation(point, -5.f, 1.6f, -3.f);
	HRL_SetLightColor(point, 0.3f, 0.5f, 1.0f);
	HRL_SetLightIntensity(point, 20.f);
	HRL_SetLightAttenuation(point, 0.15f);
	HRL_SetLightCastShadows(point, HRL_TRUE);

	const MeshData sphere = MakeSphere(0.8f, 32, 48);
	const MeshData floor = MakeBox(20.f, 0.25f, 20.f);
	const MeshData pillar = MakeBox(0.5f, 2.5f, 0.5f);
	const MeshData cube = MakeBox(0.9f, 0.9f, 0.9f);

	AddMesh(app.scene, floor, MakeMaterial(0.36f, 0.37f, 0.40f, 0.55f, 0.f), 0.f, -0.25f, 0.f);
	// Roughness along X, dielectric (front row) / metal (back row).
	for (int row = 0; row < 2; ++row)
		for (int i = 0; i < 6; ++i)
		{
			const float roughness = 0.05f + 0.19f * (float)i;
			const HRL_id mat = row == 0 ? MakeMaterial(0.85f, 0.12f, 0.10f, roughness, 0.f)
			                            : MakeMaterial(1.0f, 0.78f, 0.34f, roughness, 1.f);
			AddMesh(app.scene, sphere, mat, -5.f + 2.f * (float)i, 0.8f, row == 0 ? 2.f : -0.5f);
		}
	for (int i = 0; i < 4; ++i)
		AddMesh(app.scene, pillar, MakeMaterial(0.9f, 0.9f, 0.88f, 0.7f, 0.f), -6.f + 4.f * (float)i, 2.5f, -4.f);
	const HRL_id box = AddMesh(app.scene, cube, MakeMaterial(0.15f, 0.45f, 0.75f, 0.3f, 0.f), 3.5f, 0.9f, 5.f);
	HRL_SetMeshRotation(box, 0.f, 30.f, 0.f);
	// Emissive sphere.
	const HRL_id glow = MakeMaterial(0.02f, 0.02f, 0.02f, 0.4f, 0.f);
	HRL_MaterialSetEmissiveColor(glow, 0.2f, 1.0f, 0.5f, 4.f);
	AddMesh(app.scene, sphere, glow, -3.f, 0.8f, 5.f);

	// Translucent glass panes: blended back to front over the scene.
	const MeshData pane = MakeBox(1.1f, 0.9f, 0.03f);
	const float tints[3][3] = {{0.55f, 0.85f, 1.0f}, {1.0f, 0.75f, 0.45f}, {0.6f, 1.0f, 0.65f}};
	for (int i = 0; i < 3; ++i)
	{
		const HRL_id glass = MakeMaterial(tints[i][0], tints[i][1], tints[i][2], 0.08f, 0.f);
		HRL_MaterialSetBool(glass, HRL_MATERIAL_PARAM_TRANSLUCENT, HRL_TRUE);
		HRL_MaterialSetBool(glass, HRL_MATERIAL_PARAM_TWO_SIDED, HRL_TRUE);
		HRL_MaterialSetInt(glass, "OpacityUseValue", 1);
		HRL_MaterialSetFloat(glass, "OpacityValue", 0.35f);
		const HRL_id mesh = AddMesh(app.scene, pane, glass, -1.5f + 1.6f * (float)i, 0.9f, 3.6f + 0.5f * (float)i);
		HRL_SetMeshRotation(mesh, 0.f, -45.f, 0.f);
	}

	HRL_Vulkan_SetRayTracedShadows(opt.rayTracedShadows ? HRL_TRUE : HRL_FALSE);
}

void UpdateCamera(const App& app)
{
	HRL_SetCameraLocation(app.camera, app.camX, app.camY, app.camZ);
	HRL_SetCameraRotation(app.camera, app.pitch, app.yaw, 0.f);
}

#ifdef HRL_EXAMPLE_HAS_GLFW
// vulkan.h is not needed: the handles cross the HRL API as opaque values.
extern "C" int glfwCreateWindowSurface(void* instance, GLFWwindow* window, const void* allocator, void* surface);

int CreateSurface(void* instance, void* surfaceOut, void* user)
{
	return glfwCreateWindowSurface(instance, static_cast<GLFWwindow*>(user), nullptr, surfaceOut);
}

int RunWindow(Options& opt)
{
	if (!glfwInit()) { std::fprintf(stderr, "GLFW initialisation failed\n"); return 1; }
	if (!glfwVulkanSupported()) { std::fprintf(stderr, "No Vulkan loader / driver found\n"); glfwTerminate(); return 1; }
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	GLFWwindow* window = glfwCreateWindow(opt.width, opt.height, "HRL - Vulkan PBR", nullptr, nullptr);
	if (!window) { glfwTerminate(); return 1; }

	glfwSwapInterval(0);

	uint32_t extensionCount = 0;
	const char** extensions = glfwGetRequiredInstanceExtensions(&extensionCount);
	HRL_RegisterErrorCallback(OnError);
	HRL_Vulkan_SetInstanceExtensions(extensions, extensionCount);
	HRL_Vulkan_SetSurfaceCallback(CreateSurface, window);
	HRL_Init(HRL_VULKAN);
	int fbw = 0, fbh = 0;
	glfwGetFramebufferSize(window, &fbw, &fbh);
	HRL_InitContext((HRL_uint)fbw, (HRL_uint)fbh, nullptr);
	if (g_errors) { glfwTerminate(); return 1; }
	std::printf("GPU: %s | hardware ray tracing: %s\n", HRL_Vulkan_GetDeviceName(),
		HRL_Vulkan_IsRayTracingSupported() ? "yes" : "no");

	App app;
	BuildScene(app, opt);
	double lastTime = glfwGetTime(), lastX = 0, lastY = 0;
	bool looking = false, rWasDown = false, pWasDown = false;
	int shot = 0, frame = 0;
	while (!glfwWindowShouldClose(window) && (opt.frames == 0 || frame++ < opt.frames))
	{
		glfwPollEvents();
		const double now = glfwGetTime();
		const float dt = (float)(now - lastTime);
		lastTime = now;

		static float elapsed = 0.f;
		static int frames = 0;

		elapsed += dt;
		frames++;

		if (elapsed >= 1.0f)
		{
			char title[128];
			std::snprintf(title, sizeof(title),
				"HRL Vulkan - %d FPS", frames);

			glfwSetWindowTitle(window, title);

			elapsed = 0.f;
			frames = 0;
		}

		int w = 0, h = 0;
		glfwGetFramebufferSize(window, &w, &h);
		if (w != fbw || h != fbh)
		{
			fbw = w; fbh = h;
			HRL_WindowResizeCallback(w, h);
		}
		if (w == 0 || h == 0) { glfwWaitEvents(); continue; }

		if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(window, 1);
		double mx = 0, my = 0;
		glfwGetCursorPos(window, &mx, &my);
		const bool rmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
		if (rmb && looking)
		{
			app.yaw += (float)(mx - lastX) * 0.15f;
			app.pitch -= (float)(my - lastY) * 0.15f;
			app.pitch = app.pitch > 89.f ? 89.f : (app.pitch < -89.f ? -89.f : app.pitch);
		}
		looking = rmb;
		lastX = mx; lastY = my;

		const float p = app.pitch * kPi / 180.f, y = app.yaw * kPi / 180.f;
		const float fx = std::cos(p) * std::cos(y), fy = std::sin(p), fz = std::cos(p) * std::sin(y);
		const float rx = -std::sin(y), rz = std::cos(y);
		const float speed = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 15.f : 5.f) * dt;
		auto key = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS ? 1.f : 0.f; };
		const float forward = key(GLFW_KEY_W) - key(GLFW_KEY_S), right = key(GLFW_KEY_D) - key(GLFW_KEY_A);
		const float up = key(GLFW_KEY_E) - key(GLFW_KEY_Q);
		app.camX += (fx * forward + rx * right) * speed;
		app.camY += (fy * forward + up) * speed;
		app.camZ += (fz * forward + rz * right) * speed;

		const bool rDown = glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS;
		if (rDown && !rWasDown)
		{
			HRL_Vulkan_SetRayTracedShadows(!HRL_Vulkan_GetRayTracedShadows());
			std::printf("ray traced shadows: %s\n", HRL_Vulkan_GetRayTracedShadows() ? "on" : "off (shadow maps)");
		}
		rWasDown = rDown;
		const bool pDown = glfwGetKey(window, GLFW_KEY_P) == GLFW_PRESS;
		if (pDown && !pWasDown)
		{
			const std::string path = "hrl_screenshot_" + std::to_string(shot++) + ".png";
			HRL_TakeScreenshot(app.scene, path.c_str());
			std::printf("screenshot: %s\n", path.c_str());
		}
		pWasDown = pDown;

		UpdateCamera(app);
		HRL_BeginFrame();
		HRL_EndFrame();
	}
	HRL_Shutdown();
	glfwDestroyWindow(window);
	glfwTerminate();
	return g_errors ? 1 : 0;
}
#endif

int RunOffscreen(const Options& opt)
{
	HRL_RegisterErrorCallback(OnError);
	HRL_Init(HRL_VULKAN);
	HRL_InitContext((HRL_uint)opt.width, (HRL_uint)opt.height, nullptr);
	if (g_errors) return 1;
	std::printf("GPU: %s | hardware ray tracing: %s\n", HRL_Vulkan_GetDeviceName(),
		HRL_Vulkan_IsRayTracingSupported() ? "yes" : "no");
	if (opt.rayTracedShadows && !HRL_Vulkan_IsRayTracingSupported())
	{
		std::fprintf(stderr, "--rt requested but hardware ray tracing is not available\n");
		HRL_Shutdown();
		return 2;
	}
	App app;
	BuildScene(app, opt);
	UpdateCamera(app);
	const int frames = opt.frames > 0 ? opt.frames : 3;
	for (int i = 0; i < frames; ++i)
	{
		HRL_BeginFrame();
		if (i == frames - 1) HRL_TakeScreenshot(app.scene, opt.screenshot.c_str());
		HRL_EndFrame();
	}
	// Picking: the centre of the image looks at the sphere rows.
	HRL_EMeshType type = HRL_3D_MESH;
	const HRL_id hovered = HRL_GetHoveredObject(app.scene, opt.width / 2, opt.height / 2, &type);
	std::printf("object under the image centre: %s\n", hovered == HRL_INVALID_ID ? "none" : "a 3D mesh");
	HRL_Shutdown(); // pending screenshots are written here at the latest
	std::printf("wrote %s\n", opt.screenshot.c_str());
	return g_errors ? 1 : 0;
}
} // namespace

int main(int argc, char** argv)
{
	Options opt;
	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		if (a == "--screenshot" && i + 1 < argc) opt.screenshot = argv[++i];
		else if (a == "--size" && i + 2 < argc) { opt.width = std::atoi(argv[++i]); opt.height = std::atoi(argv[++i]); }
		else if (a == "--frames" && i + 1 < argc) opt.frames = std::atoi(argv[++i]);
		else if (a == "--rt") opt.rayTracedShadows = true;
		else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; }
	}
	if (opt.width < 16 || opt.height < 16 || opt.frames < 0) { std::fprintf(stderr, "invalid size / frame count\n"); return 1; }
	if (!opt.screenshot.empty()) return RunOffscreen(opt);
#ifdef HRL_EXAMPLE_HAS_GLFW
	return RunWindow(opt);
#else
	std::fprintf(stderr, "built without GLFW: use --screenshot <file.png>\n");
	return 1;
#endif
}
