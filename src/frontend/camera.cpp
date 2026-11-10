// HRL frontend - viewports and cameras.
#include "frontend_internal.h"

HRL_id HRL_CreateViewport(HRL_id _sceneid, HRL_id _cameraid, float x, float y, float _width, float _height)
{
	auto it_scene = ctx_.scenes.find(_sceneid);
	if (it_scene == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateViewport: invalid scene ID");
		return HRL_INVALID_ID;
	}

	auto it = ctx_.cameras.find(_cameraid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateViewport: invalid camera ID");
		return HRL_INVALID_ID;
	}
	HRL_Camera* cam = it->second;

	if (cam->scene_ != _sceneid)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_CreateViewport: camera belongs to another scene");
		return HRL_INVALID_ID;
	}

	auto* v = new HRL_Viewport(cam, x, y, _width, _height);

	HRL_id newId = GenerateHRL_ID();
	v->scene_ = _sceneid;
	it_scene->second->viewports.emplace(newId, v);
	ctx_.viewports.emplace(newId, v);

	return newId;
}

void HRL_DeleteViewport(HRL_id _viewportid)
{
	auto it = ctx_.viewports.find(_viewportid);
	if (it == ctx_.viewports.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteViewport: invalid ID");
		return;
	}

	// Destroy viewport-owned resources before the viewport itself.
	std::vector<HRL_id> gizmoIds;
	for (const auto& [gizmoId, gizmo] : ctx_.gizmos)
	{
		if (gizmo && gizmo->viewport_ == _viewportid) gizmoIds.push_back(gizmoId);
	}
	for (HRL_id id : gizmoIds) HRL_DeleteGizmo(id);

	std::vector<HRL_id> postIds;
	for (const auto& [id, pp] : it->second->post_processes)
	{ (void)pp; postIds.push_back(id); }
	for (HRL_id id : postIds) HRL_DeletePostProcess(id);

	std::vector<HRL_id> widgetIds;
	for (const auto& [id, widget] : it->second->widgets)
	{ (void)widget; widgetIds.push_back(id); }
	for (HRL_id id : widgetIds) HRL_DeleteWidget(id);

	//retire de la scene propriétaire
	for (auto& [scene_id, scene] : ctx_.scenes)
	{
		auto sit = scene->viewports.find(_viewportid);
		if (sit != scene->viewports.end())
		{
			scene->viewports.erase(sit);
			break;
		}
	}

	delete it->second;
	ctx_.viewports.erase(it);
}

void HRL_SetViewportCamera(HRL_id _viewportid, HRL_id _camid)
{
	auto viewport_it = ctx_.viewports.find(_viewportid);
	if (viewport_it == ctx_.viewports.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetViewportCamera: invalid viewport ID");
		return;
	}

	auto cam_it = ctx_.cameras.find(_camid);
	if (cam_it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetViewportCamera: invalid camera ID");
		return;
	}
	if (viewport_it->second->scene_ != HRL_INVALID_ID &&
		cam_it->second->scene_ != viewport_it->second->scene_)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetViewportCamera: camera belongs to another scene");
		return;
	}

	viewport_it->second->camera_ = cam_it->second;
}

void HRL_SetViewportRect(HRL_id _viewportid, float x, float y, float _width, float _height)
{
	auto it = ctx_.viewports.find(_viewportid);
	if (it == ctx_.viewports.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetViewportRect: invalid ID");
		return;
	}
	it->second->x_      = x;
	it->second->y_      = y;
	it->second->width_  = _width;
	it->second->height_ = _height;
}


HRL_id HRL_CreateCamera(HRL_id _sceneid, HRL_ECameraType _type)
{
	auto it_scene = ctx_.scenes.find(_sceneid);
	if (it_scene == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateCamera: invalid scene ID");
		return HRL_INVALID_ID;
	}

	if (_type == HRL_ORTHO || _type == HRL_PERSPECTIVE)
	{
		auto* cam = new HRL_Camera(
			//type, position, rotation
			_type,
			glm::vec3(1.f),
			glm::vec3(0.f),

			//fov vertical, near plane, far plane
			1000.f,
			0.01f,
			1000.f
			);
		HRL_id newId = GenerateHRL_ID();
		cam->scene_ = _sceneid;
		it_scene->second->cameras.emplace(newId, cam);
		ctx_.cameras.emplace(newId, cam);
		return newId;
	}
	else
	{
		SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_CreateCamera: invalid camera type");
		return HRL_INVALID_ID;
	}
}

void HRL_DeleteCamera(HRL_id _camid)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteCamera: invalid ID");
		return;
	}

	// Invalidate every viewport reference before deleting the camera.
	for (auto& [viewportId, viewport] : ctx_.viewports)
	{
		(void)viewportId;
		if (viewport && viewport->camera_ == it->second)
			viewport->camera_ = nullptr;
	}

	//retire de la scene propriétaire
	for (auto& [scene_id, scene] : ctx_.scenes)
	{
		auto sit = scene->cameras.find(_camid);
		if (sit != scene->cameras.end())
		{
			scene->cameras.erase(sit);
			break;
		}
	}

	delete it->second;
	ctx_.cameras.erase(it);
}


void HRL_SetCameraUserHandle(HRL_id _camid, void* _handle)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraUserHandle: invalid ID");
		return;
	}

	it->second->user_handle_ = _handle;
}

void* HRL_GetCameraUserHandle(HRL_id _camid)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetCameraUserHandle: invalid ID");
		return nullptr;
	}

	return it->second->user_handle_;
}

void HRL_SetCameraType(HRL_id _camid, HRL_ECameraType _type)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraType: invalid ID");
		return;
	}
	it->second->type_ = _type;
}

void HRL_SetCameraOrthoVertical(HRL_id _camid, float _height)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraOrthoVertical: invalid ID");
		return;
	}
	if (it->second->type_ == HRL_ORTHO)
	{
		it->second->value_ = _height;
	}
	else
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_WARNING, "HRL_SetCameraOrthoVertical: camera is not of type Ortho");
	}
}

void HRL_SetCameraPerspectiveFov(HRL_id _camid, float _fov)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraPerspectiveFov: invalid ID");
		return;
	}
	if (it->second->type_ == HRL_PERSPECTIVE)
	{
		it->second->value_ = _fov;
	}
	else
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_WARNING, "HRL_SetCameraPerspectiveFov: camera is not of type Perspective");
	}
}

void HRL_SetCameraNearPlane(HRL_id _camid, float _nearPlane)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraNearPlane: invalid ID");
		return;
	}
	it->second->near_plane_ = _nearPlane;
}

void HRL_SetCameraFarPlane(HRL_id _camid, float _farPlane)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraFarPlane: invalid ID");
		return;
	}
	it->second->far_plane_ = _farPlane;
}

void HRL_SetCameraLocation(HRL_id _camid, float x, float y, float z)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraLocation: invalid ID");
		return;
	}
	it->second->position_ = glm::vec3(x, y, z);
}

void HRL_SetCameraRotation(HRL_id _camid, float pitch, float yaw, float roll)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraRotation: invalid ID");
		return;
	}
	it->second->rotation_ = glm::vec3(pitch, yaw, roll);
}
