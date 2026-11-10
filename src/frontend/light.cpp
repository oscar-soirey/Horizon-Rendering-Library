// HRL frontend - lights.
#include "frontend_internal.h"

//Lights//
HRL_id HRL_CreateLight(HRL_id _sceneid, HRL_ELightType _type)
{
	auto it_scene = ctx_.scenes.find(_sceneid);
	if (it_scene == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateLight: invalid scene ID");
		return HRL_INVALID_ID;
	}

	auto* l = new HRL_Light();
	l->scene_ = _sceneid;
	l->type_ = _type;
	if (_type == HRL_SKY_LIGHT)
		l->intensity_ = 0.15f;

	HRL_id newId = GenerateHRL_ID();
	l->id_ = newId;
	it_scene->second->lights.emplace(newId, l);
	ctx_.lights.emplace(newId, l);
	MarkSceneGILightingDirty(it_scene->second);

	UpdateSceneLights(_sceneid);

	return newId;
}

void HRL_DeleteLight(HRL_id _lightid)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteLight: invalid ID");
		return;
	}

	const HRL_id owner_scene = it->second->scene_;
	g_Backend.RHI_DeleteLight(_lightid);
	auto scene_it = ctx_.scenes.find(owner_scene);
	if (scene_it != ctx_.scenes.end())
	{
		scene_it->second->lights.erase(_lightid);
		MarkSceneGILightingDirty(scene_it->second);
		scene_it->second->shadows_dirty = true;
	}

	delete it->second;
	ctx_.lights.erase(it);

	UpdateSceneLights(owner_scene);
}

void HRL_SetLightColor(HRL_id _lightid, float x, float y, float z)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightColor: invalid ID");
		return;
	}
	//rappel : la derniere valeur ne compte pas, elle est juste la pour des raisons techniques
	if (it->second->color_ == glm::vec3(x, y, z))
		return; // inchange : pas d'invalidation GI ni de re-upload
	it->second->color_ = glm::vec4(x, y, z, 0.f);

	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end())
		MarkSceneGILightingDirty(scene_it->second);

	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightIntensity(HRL_id _lightid, float i)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightIntensity: invalid ID");
		return;
	}
	if (it->second->intensity_ == i)
		return;
	it->second->intensity_ = i;

	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end())
		MarkSceneGILightingDirty(scene_it->second);

	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightAttenuation(HRL_id _lightid, float a)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightAttenuation: invalid ID");
		return;
	}
	if (it->second->attenuation_ == a)
		return;
	it->second->attenuation_ = a;

	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end())
		MarkSceneGILightingDirty(scene_it->second);

	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightLocation(HRL_id _lightid, float x, float y, float z)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightLocation: invalid ID");
		return;
	}
	//rappel : la derniere valeur ne compte pas, elle est juste la pour des raisons techniques
	if (it->second->position_ == glm::vec3(x, y, z))
		return; // evite de recalculer toutes les shadow maps pour rien
	it->second->position_ = glm::vec4(x, y, z, 0.f);
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end()) { MarkSceneGILightingDirty(scene_it->second); scene_it->second->shadows_dirty = true; }

	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightRotation(HRL_id _lightid, float pitch, float yaw, float roll)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightRotation: invalid ID");
		return;
	}
	//rappel : la derniere valeur ne compte pas, elle est juste la pour des raisons techniques
	if (it->second->rotation_ == glm::vec3(pitch, yaw, roll))
		return;
	it->second->rotation_ = glm::vec4(pitch, yaw, roll, 0.f);
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end()) { MarkSceneGILightingDirty(scene_it->second); scene_it->second->shadows_dirty = true; }

	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightCastShadows(HRL_id _lightid, int _enable)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightCastShadows: invalid ID");
		return;
	}
	if (it->second->cast_shadows_ == (_enable != HRL_FALSE))
		return;
	it->second->cast_shadows_ = (_enable != HRL_FALSE);
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end()) { MarkSceneGILightingDirty(scene_it->second); scene_it->second->shadows_dirty = true; }
	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightShadowBias(HRL_id _lightid, float _bias)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightShadowBias: invalid ID");
		return;
	}
	if (!std::isfinite(_bias) || _bias < 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLightShadowBias: bias must be finite and non-negative");
		return;
	}
	if (it->second->shadow_bias_ == _bias)
		return;
	it->second->shadow_bias_ = _bias;
	// Le biais est applique cote recepteur (shader d'eclairage, via l'UBO des
	// lumieres envoye a chaque frame) : inutile de re-rendre les shadow maps.
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end())
		MarkSceneGILightingDirty(scene_it->second);
}

void HRL_SetLightShadowStrength(HRL_id _lightid, float _strength)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightShadowStrength: invalid ID");
		return;
	}
	if (!std::isfinite(_strength) || _strength < 0.f || _strength > 1.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLightShadowStrength: strength must be finite and in [0, 1]");
		return;
	}
	if (it->second->shadow_strength_ == _strength)
		return;
	it->second->shadow_strength_ = _strength;
	MarkSceneGILightingDirty(ctx_.scenes.at(it->second->scene_));
	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightShadowResolution(HRL_id _lightid, int _resolution)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightShadowResolution: invalid ID");
		return;
	}
	if (_resolution < 128 || (_resolution & (_resolution - 1)) != 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLightShadowResolution: resolution must be a power of two and at least 128");
		return;
	}
	it->second->shadow_resolution_ = _resolution;
}

void HRL_SetSpotLightInnerCutoff(HRL_id _lightid, float inner_cutoff)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSpotLightInnerCutoff: invalid ID");
		return;
	}
	if (it->second->type_ != HRL_SPOT_LIGHT)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSpotLightInnerCutoff: light is not of type SpotLight");
		return;
	}

	if (it->second->innerCutoff == inner_cutoff)
		return;
	it->second->innerCutoff = inner_cutoff;
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end()) { MarkSceneGILightingDirty(scene_it->second); scene_it->second->shadows_dirty = true; }

	UpdateSceneLights(it->second->scene_);
}

void HRL_SetSpotLightOuterCutoff(HRL_id _lightid, float outer_cutoff)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSpotLightOuterCutoff: invalid ID");
		return;
	}
	if (it->second->type_ != HRL_SPOT_LIGHT)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSpotLightOuterCutoff: light is not of type SpotLight");
		return;
	}

	if (it->second->outerCutoff == outer_cutoff)
		return;
	it->second->outerCutoff = outer_cutoff;
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end()) { MarkSceneGILightingDirty(scene_it->second); scene_it->second->shadows_dirty = true; }

	UpdateSceneLights(it->second->scene_);
}
