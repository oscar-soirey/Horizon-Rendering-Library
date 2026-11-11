// Services of the HRL core that a rendering backend may use.
//
// A backend receives everything it renders through the functions of
// backend_vtable.h (resources) and through HRL_SceneFrame (the scene to draw).
// It must not reach into the frontend state: the native Vulkan backend only
// includes this header, render_common.h and object_types.h from the core
// (checked by the `backend_isolation` test).
#ifndef HRL_BACKEND_API_H
#define HRL_BACKEND_API_H

#include "../hrl.h"
#include "id_map.h"
#include "object_types.h"

#include <string>
#include <vector>

#include <glm/glm.hpp>

// Reports an error or a warning to the application (HRL_GetLastError and the error callback).
void SetErrorCode(HRL_EError e, HRL_ESeverity severity, const std::string& detail);

// New identifier for an object created by a backend (texture...).
HRL_id GenerateHRL_ID();

// Orientation vectors of HRL euler angles (pitch, yaw, roll in degrees).
glm::vec3 GetForwardVector(glm::vec3 _rotation);
glm::vec3 GetRightVector(glm::vec3 _rotation);
glm::vec3 GetUpVector(glm::vec3 _rotation);

// A textured, tinted rectangle drawn over the finished image of a viewport
// (widgets, texts, on-screen messages). Coordinates are normalized to the
// viewport, origin at its top-left corner. Texture coordinates follow the
// texture convention of HRL (v = 0 is the first row stored, the bottom of an
// image loaded with HRL_CreateTexture).
struct HRL_OverlayQuad
{
	float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
	float u0 = 0.f, v0 = 1.f;   // at the top-left corner
	float u1 = 1.f, v1 = 0.f;   // at the bottom-right corner
	glm::vec4 color{1.f};
	HRL_id texture = HRL_INVALID_ID; // HRL_INVALID_ID: plain colour
	bool sdf = false;                // the red channel is a signed distance (text)
};

// Overlay of one viewport, drawn in order after its gizmos.
struct HRL_ViewportOverlay
{
	HRL_id viewport = HRL_INVALID_ID;
	std::vector<HRL_OverlayQuad> quads;
};

// One scene to render this frame, as handed by the frontend to
// HRL_vtable::RHI_RenderSceneFrame. The pointers are valid during the call only.
struct HRL_SceneFrame
{
	HRL_id sceneId = HRL_INVALID_ID;
	const hrl_scene_t* scene = nullptr;
	// Every material of the application, by id. HRL_Material::revision_ changes
	// whenever a material is modified.
	const HRL_IdMap<HRL_Material*>* materials = nullptr;
	// Debug primitives submitted for this scene this frame (may be null).
	const DebugRenderer* debug = nullptr;
	float debugLineThickness = 1.f;
	// When not null, the finished image of the scene is written to this PNG file.
	const char* screenshotPath = nullptr;
	// 2D overlays of the viewports (may be null). Gizmos are built by the backend
	// from the scene (BuildGizmoOverlay), with its own camera matrices.
	const std::vector<HRL_ViewportOverlay>* overlays = nullptr;
	// Window size in pixels, for the overlays sized in pixels.
	uint32_t windowWidth = 0, windowHeight = 0;
};

#endif
