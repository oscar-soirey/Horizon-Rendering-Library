// HRL frontend - textures, text textures, fonts.
#include "frontend_internal.h"

//Textures//
HRL_id HRL_CreateTexture(const char* _fileContent, size_t _bufferSize)
{
	//gestion des erreurs auto par le backend
	return g_Backend.RHI_CreateTexture(_fileContent, _bufferSize);
}


HRL_id HRL_CreateTextureAsync(const char* data, size_t size)
{
  if (!data || size == 0) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateTextureAsync: invalid data buffer"); return HRL_INVALID_ID; }
  const HRL_id id = GenerateHRL_ID();
  ctx_.async_resource_states[id] = static_cast<int>(AsyncResourceState::Pending);
  AsyncResourceTask task; task.type = AsyncResourceResult::Type::Texture; task.id = id;
  task.bytes.assign(reinterpret_cast<const unsigned char*>(data), reinterpret_cast<const unsigned char*>(data) + size);
  g_AsyncResourceLoader.Enqueue(std::move(task));
  return id;
}


int HRL_IsTextureReady(HRL_id id)
{
  auto it = ctx_.async_resource_states.find(id);
  if (it == ctx_.async_resource_states.end()) return g_Backend.RHI_IsValidTexture ? g_Backend.RHI_IsValidTexture(id) : HRL_FALSE;
  return it->second == static_cast<int>(AsyncResourceState::Ready) && g_Backend.RHI_IsValidTexture(id);
}

void HRL_WaitForTexture(HRL_id id)
{
  for (;;) { ProcessAsyncResourceUploads(); auto it = ctx_.async_resource_states.find(id); if (it == ctx_.async_resource_states.end()) return; if (it->second != static_cast<int>(AsyncResourceState::Pending)) return; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
void HRL_DeleteTexture(HRL_id _textureid)
{
  auto asyncIt = ctx_.async_resource_states.find(_textureid);
  if (asyncIt != ctx_.async_resource_states.end()) {
    if (asyncIt->second == static_cast<int>(AsyncResourceState::Pending)) { asyncIt->second = static_cast<int>(AsyncResourceState::Cancelled); ctx_.async_resource_states.erase(asyncIt); return; }
    ctx_.async_resource_states.erase(asyncIt);
  }
  g_Backend.RHI_DeleteTexture(_textureid);
	MarkAllScenesGILightingDirty();
}


void HRL_GetTextureSize(HRL_id _textureid, int *_width, int *_height)
{
	if (_width && _height)
	{
		g_Backend.RHI_GetTextureSize(_textureid, _width, _height);
	}
	else
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetTextureSize: width or height are not a valid pointer");
	}
}

void HRL_SetTextureMinFilter(HRL_id _textureid, HRL_EFilterType _filter)
{
	g_Backend.RHI_SetTextureMinFilter(_textureid, _filter);
}
void HRL_SetTextureMagFilter(HRL_id _textureid, HRL_EFilterType _filter)
{
	g_Backend.RHI_SetTextureMaxFilter(_textureid, _filter);
}

HRL_id HRL_InternalCreateSDFTextTexture(const char* _text, HRL_id _fontid, float _font_size)
{
	auto it = ctx_.fonts.find(_fontid);
	if (it == ctx_.fonts.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_InternalCreateSDFTextTexture: invalid font ID");
		return HRL_INVALID_ID;
	}

	BitmapResult bmp = GenerateSDFBitmap(_text, &it->second->info, it->second->ttf_buffer, _font_size, 0.0f);
	if (bmp.pixels.empty())
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_InternalCreateSDFTextTexture: SDF generation failed");
		return HRL_INVALID_ID;
	}

	return g_Backend.RHI_CreateTextureFromBitmap(bmp);
}

HRL_API HRL_id HRL_CreateTextureFromText(const char* _text, HRL_id _fontid,
	float _font_size, float _wrap_width,
	float r, float g, float b,
	float bg_r, float bg_g, float bg_b, float bg_a
)
{
	auto it = ctx_.fonts.find(_fontid);
	if (it == ctx_.fonts.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateTextureFromText: invalid font ID");
		return HRL_INVALID_ID;
	}

	BitmapResult bmp = GenerateBitmap(_text, &it->second->info, it->second->ttf_buffer,
			_font_size, _wrap_width,
			r, g, b,
			bg_r, bg_g, bg_b, bg_a
	);
	if (bmp.pixels.empty())
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_CreateTextureFromText: bitmap generation failed, pixels is empty");
		return HRL_INVALID_ID;
	}

	return g_Backend.RHI_CreateTextureFromBitmap(bmp);
}


void HRL_ClearScreen()
{
	/**for (const auto& s : ctx_.scenes)
	{
		if (!s.second)
		{
			SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_ClearScreen: tried to clear an invalid scene object");
			return;
		}
		g_Backend.RHI_BindScene(s.first);
		g_Backend.RHI_ResetFramebuffer();
	}*/
}


//Text//
HRL_id HRL_CreateFont(const char *data, size_t _data_size)
{
	HRL_id newId = GenerateHRL_ID();
	auto* font = new HRL_Font();

	font->ttf_buffer.assign(data, data+_data_size);

	int ok = stbtt_InitFont(
		&font->info,
		font->ttf_buffer.data(),
		stbtt_GetFontOffsetForIndex(font->ttf_buffer.data(), 0)
	);

	if (!ok)
	{
		SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_CreateFont: failed to parse TTF data");
		delete font;
		return HRL_INVALID_ID;
	}

	ctx_.fonts.emplace(newId, font);

	return newId;
}

void HRL_DeleteFont(HRL_id _fontid)
{
	auto it = ctx_.fonts.find(_fontid);
	if (it == ctx_.fonts.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteFont: invalid ID");
		return;
	}
	delete it->second;
	ctx_.fonts.erase(it);
}
