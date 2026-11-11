// Off-screen test of the 2D features of the Vulkan backend: sprites (regions,
// draw order, picking), widgets (input and drawing), SDF texts, on-screen
// messages and gizmos. The pixels of screenshots are checked.
#include "test_common.h"

#include <hrl_vulkan.h>

#include <fstream>
#include <iterator>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

namespace
{
struct Picture
{
	int w = 0, h = 0;
	std::vector<unsigned char> rgba;
	// Pixel at (x, y), origin top-left.
	const unsigned char* At(int x, int y) const { return &rgba[((size_t)y * w + x) * 4]; }
};

bool Load(const char* path, Picture& p)
{
	int c = 0;
	unsigned char* data = stbi_load(path, &p.w, &p.h, &c, 4);
	if (!data) return false;
	p.rgba.assign(data, data + (size_t)p.w * p.h * 4);
	stbi_image_free(data);
	return true;
}

Picture Shot(HRL_id scene, const char* path)
{
	HRL_BeginFrame();
	HRL_TakeScreenshot(scene, path);
	HRL_EndFrame();
	Frames(3); // written once the GPU is done
	Picture p;
	CHECK(Load(path, p));
	return p;
}

bool Near(const unsigned char* px, int r, int g, int b, int tolerance = 12)
{
	return std::abs(px[0] - r) <= tolerance && std::abs(px[1] - g) <= tolerance && std::abs(px[2] - b) <= tolerance;
}

HRL_id SolidTexture(int w, int h, unsigned char r, unsigned char g, unsigned char b, bool rightHalfTransparent)
{
	std::vector<unsigned char> px((size_t)w * h * 4);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
		{
			unsigned char* p = &px[((size_t)y * w + x) * 4];
			p[0] = r; p[1] = g; p[2] = b;
			p[3] = (rightHalfTransparent && x >= w / 2) ? 0 : 255;
		}
	return HRL_CreateTextureFromPixels(px.data(), w, h);
}

int g_clicks = 0;
void OnClick(HRL_id, int clicked, int, void*) { if (clicked) ++g_clicks; }
} // namespace

int main()
{
	HRL_RegisterErrorCallback(OnError);
	HRL_Init(HRL_VULKAN);
	HRL_InitContext(320, 200, nullptr);
	if (g_errors) { std::fprintf(stderr, "Vulkan initialisation failed\n"); return 1; }

	const HRL_id scene = HRL_CreateScene(HRL_TRUE);
	const HRL_id camera = HRL_CreateCamera(scene, HRL_ORTHO);
	HRL_SetCameraOrthoVertical(camera, 10.f); // 10 units high, 16 wide
	HRL_SetCameraNearPlane(camera, 0.1f);
	HRL_SetCameraFarPlane(camera, 100.f);
	HRL_SetCameraLocation(camera, 0.f, 0.f, 10.f);
	HRL_SetCameraRotation(camera, 0.f, -90.f, 0.f); // towards -Z
	const HRL_id viewport = HRL_CreateViewport(scene, camera, 0.f, 0.f, 1.f, 1.f);
	const HRL_id sky = HRL_CreateLight(scene, HRL_SKY_LIGHT);
	HRL_SetLightIntensity(sky, 1.f);
	// 2D: colours as given, no tone mapping (as the OpenGL default).
	const HRL_id postMat = HRL_CreateMaterial(HRL_DEFAULT_POST_PROCESS_SHADER);
	HRL_MaterialSetInt(postMat, "toneMappingEnabled", 0);
	HRL_MaterialSetFloat(postMat, "bloomStrength", 0.f);
	HRL_CreatePostProcess(viewport, postMat, 0);

	// ---- Sprites ----------------------------------------------------------------
	// Red, right half transparent: only the left half is drawn and pickable.
	const HRL_id redTexture = SolidTexture(16, 16, 255, 0, 0, true);
	const HRL_id redMat = HRL_CreateMaterial(HRL_SPRITE_SHADER);
	HRL_MaterialSetTexture(redMat, HRL_T_ALBEDO, redTexture);
	const HRL_id red = HRL_CreateMeshSprite(scene);
	CHECK(red != HRL_INVALID_ID);
	HRL_SetMeshMaterial(red, redMat);
	HRL_SetMeshScale(red, 4.f, 4.f, 1.f);
	HRL_SetMeshLocation(red, -4.f, 0.f, 0.f);
	// Blue sprite with a lower draw order, under a green one at the same place.
	const HRL_id blueMat = HRL_CreateMaterial(HRL_SPRITE_SHADER);
	HRL_MaterialSetTexture(blueMat, HRL_T_ALBEDO, SolidTexture(4, 4, 0, 0, 255, false));
	const HRL_id greenMat = HRL_CreateMaterial(HRL_SPRITE_SHADER);
	HRL_MaterialSetTexture(greenMat, HRL_T_ALBEDO, SolidTexture(4, 4, 0, 255, 0, false));
	const HRL_id blue = HRL_CreateMeshSprite(scene);
	const HRL_id green = HRL_CreateMeshSprite(scene);
	HRL_SetMeshMaterial(blue, blueMat);
	HRL_SetMeshMaterial(green, greenMat);
	for (HRL_id s : {blue, green})
	{
		HRL_SetMeshScale(s, 3.f, 3.f, 1.f);
		HRL_SetMeshLocation(s, 4.f, 0.f, 0.f);
	}
	HRL_SetSpriteDrawOrder(blue, 2.f);
	HRL_SetSpriteDrawOrder(green, 1.f);

	Frames(2);
	CHECK(g_errors == 0);
	// Image: 320 x 200 for 16 x 10 units, 20 px per unit, centre (160, 100).
	Picture p = Shot(scene, "hrl_test_2d_sprites.png");
	if (!p.rgba.empty())
	{
		CHECK(Near(p.At(160 - 4 * 20 - 20, 100), 255, 0, 0));  // left half of the red sprite
		CHECK(!Near(p.At(160 - 4 * 20 + 20, 100), 255, 0, 0)); // transparent right half
		CHECK(Near(p.At(160 + 4 * 20, 100), 0, 0, 255));       // blue: drawn last (higher draw order)
	}
	HRL_EMeshType type = HRL_3D_MESH;
	CHECK(HRL_GetHoveredObject(scene, 160 - 4 * 20 - 20, 100, &type) == red);
	CHECK(type == HRL_SPRITE);
	CHECK(HRL_GetHoveredObject(scene, 160 - 4 * 20 + 20, 100, &type) == HRL_INVALID_ID);

	// Region: the right half of the texture only (transparent): the sprite disappears.
	HRL_SetSpriteRegion(red, 0.5f, 0.f, 1.f, 1.f);
	HRL_SetSpriteDrawOrder(blue, 0.f);
	Frames(1);
	p = Shot(scene, "hrl_test_2d_region.png");
	if (!p.rgba.empty())
	{
		CHECK(!Near(p.At(160 - 4 * 20 - 20, 100), 255, 0, 0));
		CHECK(Near(p.At(160 + 4 * 20, 100), 0, 255, 0)); // green now drawn last
	}
	HRL_SetSpriteRegion(red, 0.f, 0.f, 1.f, 1.f);

	// ---- Widgets ------------------------------------------------------------------
	const HRL_id image = HRL_CreateWidget(viewport, HRL_WIDGET_IMAGE);
	HRL_SetWidgetPosition(image, 0.05f, 0.75f);
	HRL_SetWidgetSize(image, 0.2f, 0.2f); // 64 x 40 px from (16, 150)
	HRL_SetImageTintColor(image, 1.f, 1.f, 0.f, 1.f);
	const HRL_id bar = HRL_CreateWidget(viewport, HRL_WIDGET_PROGRESSBAR);
	HRL_SetWidgetPosition(bar, 0.5f, 0.85f);
	HRL_SetWidgetSize(bar, 0.4f, 0.1f); // 128 x 20 px from (160, 170)
	HRL_SetProgressBarBackgroundColor(bar, 0.f, 0.f, 0.f, 1.f);
	HRL_SetProgressBarFillColor(bar, 1.f, 0.f, 1.f, 1.f);
	HRL_SetProgressBarValue(bar, 0.5f);
	const HRL_id button = HRL_CreateWidget(viewport, HRL_WIDGET_BUTTON);
	HRL_SetWidgetPosition(button, 0.7f, 0.1f);
	HRL_SetWidgetSize(button, 0.2f, 0.15f); // 64 x 30 px from (224, 20)
	HRL_SetButtonPressedCallback(button, OnClick, nullptr);

	// Text: a label, an on-screen message (needs a font).
	std::ifstream fontFile(HRL_TEST_FONT, std::ios::binary);
	const std::vector<char> fontData((std::istreambuf_iterator<char>(fontFile)), std::istreambuf_iterator<char>());
	CHECK(!fontData.empty());
	const HRL_id font = HRL_CreateFont(fontData.data(), fontData.size());
	CHECK(font != HRL_INVALID_ID);
	const HRL_id label = HRL_CreateWidget(viewport, HRL_WIDGET_LABEL);
	HRL_SetWidgetPosition(label, 0.3f, 0.3f);
	HRL_SetWidgetSize(label, 0.4f, 0.15f);
	HRL_SetLabelFont(label, font);
	HRL_SetLabelText(label, "HRL Vulkan");
	HRL_SetLabelTintColor(label, 0.f, 1.f, 1.f, 1.f);
	HRL_SetScreenMessageFont(scene, font);
	HRL_AddScreenMessageText(scene, 30.f, "Message");
	HRL_id gizmo = HRL_CreateGizmo(viewport);
	CHECK(gizmo != HRL_INVALID_ID);
	HRL_SetGizmoPosition(gizmo, 0.f, -3.f, 0.f);

	Frames(2);
	p = Shot(scene, "hrl_test_2d_widgets.png");
	if (!p.rgba.empty())
	{
		CHECK(Near(p.At(16 + 32, 150 + 20), 255, 255, 0));    // image widget
		CHECK(Near(p.At(160 + 32, 170 + 10), 255, 0, 255));  // filled half of the progress bar
		CHECK(Near(p.At(160 + 96, 170 + 10), 0, 0, 0));      // empty half
		// Text pixels: some cyan in the label box, some white in the message.
		int cyan = 0, white = 0;
		for (int y = 60; y < 90; ++y)
			for (int x = 96; x < 224; ++x)
				if (Near(p.At(x, y), 0, 255, 255, 60)) ++cyan;
		for (int y = 14; y < 36; ++y)
			for (int x = 14; x < 120; ++x)
				if (Near(p.At(x, y), 255, 255, 255, 30)) ++white;
		CHECK(cyan > 20);
		CHECK(white > 10);
	}

	// Button click: press and release over it.
	HRL_MouseMovedCallback(224 + 32, 20 + 15);
	Frames(1);
	HRL_MouseButtonCallback(0, 1);
	Frames(1);
	HRL_MouseButtonCallback(0, 0);
	Frames(1);
	CHECK(g_clicks == 1);
	CHECK(HRL_IsWidgetHovered(button) == HRL_TRUE);

	// ---- 2D voxel world: a blue block lit by the sky light, an emissive orange row --
	{
		const HRL_id voxels = HRL_CreateScene(HRL_FALSE);
		HRL_ResizeSceneTexture(voxels, 160, 120);
		const HRL_id vcam = HRL_CreateCamera(voxels, HRL_ORTHO);
		HRL_SetCameraOrthoVertical(vcam, 12.f);
		HRL_SetCameraNearPlane(vcam, 0.1f);
		HRL_SetCameraFarPlane(vcam, 100.f);
		HRL_SetCameraLocation(vcam, 8.f, 6.f, 10.f);
		HRL_SetCameraRotation(vcam, 0.f, -90.f, 0.f);
		HRL_CreateViewport(voxels, vcam, 0.f, 0.f, 1.f, 1.f);
		const HRL_id vsky = HRL_CreateLight(voxels, HRL_SKY_LIGHT);
		HRL_SetLightIntensity(vsky, 1.f);
		const HRL_id vpost = HRL_CreateMaterial(HRL_DEFAULT_POST_PROCESS_SHADER);
		HRL_MaterialSetInt(vpost, "toneMappingEnabled", 0);
		HRL_CreatePostProcess(HRL_CreateViewport(voxels, vcam, 0.f, 0.f, 1.f, 1.f), vpost, 0);
		CHECK(HRL_CreateVoxelWorld(voxels, 64, 64) == HRL_TRUE);
		HRL_SetVoxelTypeColor(voxels, 1, 0.f, 0.f, 1.f, 1.f);
		HRL_SetVoxelTypeColor(voxels, 2, 0.f, 0.f, 0.f, 1.f);
		HRL_SetVoxelTypeEmissiveColor(voxels, 2, 1.f, 0.5f, 0.f);
		HRL_BeginVoxelEdit(voxels);
		for (int y = 2; y < 8; ++y)
			for (int x = 2; x < 8; ++x) HRL_SetVoxelType(voxels, x, y, 1);
		for (int x = 10; x < 14; ++x) HRL_SetVoxelType(voxels, x, 4, 2);
		HRL_EndVoxelEdit(voxels);
		Frames(2);
		CHECK(g_errors == 0);
		Picture vp = Shot(voxels, "hrl_test_2d_voxels.png");
		if (!vp.rgba.empty())
		{
			// 120 px for 12 units: 10 px per voxel, x = 8 at the centre (80), y = 6 at 60.
			auto px = [&](float wx, float wy) { return vp.At((int)(80 + (wx - 8.f) * 10.f), (int)(60 - (wy - 6.f) * 10.f)); };
			std::printf("voxels: block %d %d %d, emissive %d %d %d\n", px(5, 5)[0], px(5, 5)[1], px(5, 5)[2],
				px(12, 4.5f)[0], px(12, 4.5f)[1], px(12, 4.5f)[2]);
			CHECK(px(5, 5)[2] > 150 && px(5, 5)[0] < 40);
			CHECK(px(12, 4.5f)[0] > 200 && px(12, 4.5f)[1] > 80 && px(12, 4.5f)[2] < 60);
			CHECK(Near(px(14.5f, 9.5f), 0, 0, 0)); // empty voxels
		}
		// Light of the emissive voxels: the side of the block near the emitters turns orange.
		HRL_SetVoxelTypeColor(voxels, 1, 0.3f, 0.3f, 0.3f, 1.f); // grey: it reflects the orange light
		HRL_SetVoxelEmissiveLighting(voxels, HRL_TRUE, 4.f, 4.f);
		Frames(1);
		vp = Shot(voxels, "hrl_test_2d_voxel_light.png");
		if (!vp.rgba.empty())
		{
			const unsigned char* nearPx = vp.At((int)(80 + (7.5f - 8.f) * 10.f), (int)(60 - (4.5f - 6.f) * 10.f));
			const unsigned char* farPx = vp.At((int)(80 + (2.5f - 8.f) * 10.f), (int)(60 - (7.5f - 6.f) * 10.f));
			std::printf("voxel light: near %d %d %d, far %d %d %d\n", nearPx[0], nearPx[1], nearPx[2], farPx[0], farPx[1], farPx[2]);
			CHECK(nearPx[0] > nearPx[2] + 20 && nearPx[0] > farPx[0] + 10);
		}
		HRL_SetVoxelEmissiveLighting(voxels, HRL_FALSE, 1.f, 1.f);
		HRL_SetVoxelTypeColor(voxels, 1, 0.f, 0.f, 1.f, 1.f);

		// An edit is seen at the next frame.
		HRL_SetVoxelType(voxels, 5, 5, 0);
		Frames(1);
		vp = Shot(voxels, "hrl_test_2d_voxels_edit.png");
		if (!vp.rgba.empty())
			CHECK(vp.At((int)(80 + (5.5f - 8.f) * 10.f), (int)(60 - (5.5f - 6.f) * 10.f))[2] < 120); // the hole (bloom around)
		HRL_DeleteScene(voxels);
		Frames(2);
	}

	HRL_DeleteWidget(image);
	HRL_DeleteGizmo(gizmo);
	HRL_DeleteMesh(green);
	Frames(2);
	HRL_Shutdown();
	std::printf("%d check failure(s), %d HRL error(s)\n", g_failures, g_errors);
	return (g_failures || g_errors) ? 1 : 0;
}
