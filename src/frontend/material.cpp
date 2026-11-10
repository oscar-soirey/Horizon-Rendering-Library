// HRL frontend - shaders and materials.
#include "frontend_internal.h"

//Shaders//
HRL_id HRL_CreateShader(const char *_vertContent, size_t _vertSize, const char *_fragContent, size_t _fragSize)
{
	//on return directement l'id, le backend gere les erreurs et retourne InvalidID en cas d'erreur
	return g_Backend.RHI_CreateShader(_vertContent, _vertSize, _fragContent, _fragSize);
}


HRL_id HRL_CreateShaderAsync(const char* vertData, size_t vertSize, const char* fragData, size_t fragSize)
{
  if (!vertData || !fragData || vertSize == 0 || fragSize == 0) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateShaderAsync: invalid shader source buffer"); return HRL_INVALID_ID; }
  const HRL_id id = GenerateHRL_ID();
  ctx_.async_resource_states[id] = static_cast<int>(AsyncResourceState::Pending);
  AsyncResourceTask task; task.type = AsyncResourceResult::Type::Shader; task.id = id; task.vert.assign(vertData, vertSize); task.frag.assign(fragData, fragSize);
  g_AsyncResourceLoader.Enqueue(std::move(task));
  return id;
}


int HRL_IsShaderReady(HRL_id id)
{
  auto it = ctx_.async_resource_states.find(id);
  if (it == ctx_.async_resource_states.end()) return g_Backend.RHI_IsValidShader ? g_Backend.RHI_IsValidShader(id) : HRL_FALSE;
  return it->second == static_cast<int>(AsyncResourceState::Ready) && g_Backend.RHI_IsValidShader(id);
}

void HRL_WaitForShader(HRL_id id)
{
  for (;;) { ProcessAsyncResourceUploads(); auto it = ctx_.async_resource_states.find(id); if (it == ctx_.async_resource_states.end()) return; if (it->second != static_cast<int>(AsyncResourceState::Pending)) return; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}

void HRL_WaitForAllAsyncResources()
{
  for (;;) {
    ProcessAsyncResourceUploads();
    bool pending = false;
    for (const auto& [id, state] : ctx_.async_resource_states) { (void)id; if (state == static_cast<int>(AsyncResourceState::Pending)) { pending = true; break; } }
    if (!pending && !g_AsyncResourceLoader.HasPendingWork()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void HRL_DeleteShader(HRL_id _shaderid)
{
  auto asyncIt = ctx_.async_resource_states.find(_shaderid);
  if (asyncIt != ctx_.async_resource_states.end()) {
    if (asyncIt->second == static_cast<int>(AsyncResourceState::Pending)) { asyncIt->second = static_cast<int>(AsyncResourceState::Cancelled); ctx_.async_resource_states.erase(asyncIt); return; }
    ctx_.async_resource_states.erase(asyncIt);
  }
  g_Backend.RHI_DeleteShader(_shaderid);
}


//Materials//
HRL_id HRL_CreateMaterial(HRL_id _shaderid)
{
	auto* m = new HRL_Material();
	m->shader_ = _shaderid;

	HRL_id newId = GenerateHRL_ID();
	ctx_.materials.emplace(newId, m);

	return newId;
}

void HRL_DeleteMaterial(HRL_id _matid)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteMaterial: invalid ID");
		return;
	}
	if (it->second)
	{
		for (HRL_id textureId : it->second->owned_textures_)
		{
			if (textureId != HRL_INVALID_ID)
				HRL_DeleteTexture(textureId);
		}
	}
	delete it->second;
	ctx_.materials.erase(it);
	MarkAllScenesGILightingDirty();
}

void HRL_SetMaterialUserHandle(HRL_id _matid, void* _handle)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMaterialUserHandle: invalid ID");
		return;
	}

	it->second->user_handle_ = _handle;
}

void* HRL_GetMaterialUserHandle(HRL_id _matid)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetMaterialUserHandle: invalid ID");
		return nullptr;
	}

	return it->second->user_handle_;
}

void HRL_MaterialSetInt(HRL_id _matid, const char* _uniformName, int a)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetInt: invalid ID");
		return;
	}
	//si la clée n'existe pas, elle est créée
	if (!AssignMaterialParam(it->second->intParams_, _uniformName, a))
		return;
	UpdateMaterialIntFlags(it->second, _uniformName, a);
	MarkScenesUsingMaterialGILightingDirty(_matid);
}

void HRL_MaterialSetTexture(HRL_id _matid, const char* _uniformName, HRL_id _textureid)
{
	auto it_mat = ctx_.materials.find(_matid);
	if (it_mat == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetTexture: invalid material ID");
		return;
	}

	//on vérifie que la texture existe au moment de RHI_BindMaterial, car ici on a pas acces aux textures
	if (!AssignMaterialParam(it_mat->second->textureParams_, _uniformName, _textureid))
		return;
	MarkScenesUsingMaterialGILightingDirty(_matid);
}

void HRL_MaterialSetBool(HRL_id _matid, const char* _uniformName, int a)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetBool: invalid ID");
		return;
	}
	if (!AssignMaterialParam(it->second->intParams_, _uniformName, a))
		return;
	UpdateMaterialIntFlags(it->second, _uniformName, a);
	MarkScenesUsingMaterialGILightingDirty(_matid);
}

void HRL_MaterialSetFloat(HRL_id _matid, const char* _uniformName, float a)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetFloat: invalid ID");
		return;
	}
	if (!AssignMaterialParam(it->second->floatParams_, _uniformName, a))
		return;
	MarkScenesUsingMaterialGILightingDirty(_matid);
}

void HRL_MaterialSetVec2(HRL_id _matid, const char* _uniformName, float x, float y)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetVec2: invalid ID");
		return;
	}
	if (!AssignMaterialParam(it->second->vec2Params_, _uniformName, glm::vec2(x, y)))
		return;
	MarkScenesUsingMaterialGILightingDirty(_matid);
}

void HRL_MaterialSetVec3(HRL_id _matid, const char* _uniformName, float x, float y, float z)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetVec3: invalid ID");
		return;
	}
	if (!AssignMaterialParam(it->second->vec3Params_, _uniformName, glm::vec3(x, y, z)))
		return;
	MarkScenesUsingMaterialGILightingDirty(_matid);
}

void HRL_MaterialSetVec4(HRL_id _matid, const char* _uniformName, float x, float y, float z, float w)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetVec4: invalid ID");
		return;
	}
	if (!AssignMaterialParam(it->second->vec4Params_, _uniformName, glm::vec4(x, y, z, w)))
		return;
	MarkScenesUsingMaterialGILightingDirty(_matid);
}


void HRL_MaterialSetEmissiveColor(HRL_id matid, float r, float g, float b, float a)
{
	auto it = ctx_.materials.find(matid);
	if (it == ctx_.materials.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetEmissiveColor: invalid material ID");
		return;
	}
	if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) || !std::isfinite(a))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_MaterialSetEmissiveColor: color contains NaN or infinity");
		return;
	}
	// rgb : linear color, a : intensity multiplier. Read by the Vulkan renderer.
	it->second->vec4Params_["EmissiveColor"] = glm::vec4(r, g, b, a);
	MarkScenesUsingMaterialGILightingDirty(matid);
}
