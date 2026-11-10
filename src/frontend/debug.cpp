// HRL frontend - debug drawing, debug views, screen messages.
#include "frontend_internal.h"

void HRL_SetDebugLineThickness(float a)
{
	ctx_.debug_line_thickness = a;
}

void HRL_DrawDebugSegment(HRL_id _sceneid, float a_x, float a_y, float a_z, float b_x, float b_y, float b_z, float r, float g, float b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugSegment: invalid scene ID");
		return;
	}

	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x, a_y, a_z, r, g, b);
	ctx_.debug_renderers[_sceneid].lines.emplace_back(b_x, b_y, b_z, r, g, b);
}

void HRL_DrawDebugPolygon(HRL_id _sceneid, HRL_EDebugRenderingType _mode, const float *vertices_x, const float *vertices_y, const float *vertices_z, int vertices_count, float r, float g, float b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugPolygon: invalid scene ID");
		return;
	}

	if (_mode == HRL_DEBUG_SOLID)
	{
		for (int i=1; i < vertices_count-1; i++)
		{
			//pivot
			ctx_.debug_renderers[_sceneid].triangles.emplace_back(vertices_x[0], vertices_y[0], vertices_z[0], r, g, b);
			//i
			ctx_.debug_renderers[_sceneid].triangles.emplace_back(vertices_x[i], vertices_y[i], vertices_z[i], r, g, b);
			//i+1
			ctx_.debug_renderers[_sceneid].triangles.emplace_back(vertices_x[i+1], vertices_y[i+1], vertices_z[i+1], r, g, b);
		}
	}
	else if (_mode == HRL_DEBUG_HOLLOW)
	{
		for (int i=0; i < vertices_count - 1; i++)
		{
			ctx_.debug_renderers[_sceneid].lines.emplace_back(vertices_x[i], vertices_y[i], vertices_z[i], r, g, b);
			ctx_.debug_renderers[_sceneid].lines.emplace_back(vertices_x[i+1], vertices_y[i+1], vertices_z[i+1], r, g, b);
		}

		//line entre le dernier et le 0 pour refermer
		ctx_.debug_renderers[_sceneid].lines.emplace_back(vertices_x[vertices_count-1], vertices_y[vertices_count-1], vertices_z[vertices_count-1], r, g, b);
		ctx_.debug_renderers[_sceneid].lines.emplace_back(vertices_x[0], vertices_y[0], vertices_z[0], r, g, b);
	}
	else
	{
		SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_DrawDebugPolygon: invalid mode, expected HRL_DebugSolid or HRL_DebugHollow");
	}
}

void HRL_DrawDebugCircle(HRL_id _sceneid, HRL_EDebugRenderingType _mode, float center_x, float center_y, float center_z, float radius, int segments, float r, float g, float b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugCircle: invalid scene ID"); return; }

	int seg = segments;
	auto* vx = (float*)alloca(seg * sizeof(float));
	auto* vy = (float*)alloca(seg * sizeof(float));
	auto* vz = (float*)alloca(seg * sizeof(float));

	for (int i = 0; i < seg; i++)
	{
		float angle = (float)(2.f * M_PI * i) / seg;
		vx[i] = center_x + radius * cosf(angle);
		vy[i] = center_y + radius * sinf(angle);
		vz[i] = center_z;
	}

	HRL_DrawDebugPolygon(_sceneid, _mode, vx, vy, vz, seg, r, g, b);
}

void HRL_DrawDebugCapsule(HRL_id _sceneid, HRL_EDebugRenderingType _mode, float a_x, float a_y, float a_z, float b_x, float b_y, float b_z, float radius, int segments, float r, float g, float b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugCapsule: invalid scene ID"); return; }

	int half_seg = segments / 2;

	// direction A→B
	float dx = b_x - a_x;
	float dy = b_y - a_y;
	float len = sqrtf(dx * dx + dy * dy);

	// vecteur unitaire perpendiculaire
	float nx = 0.f, ny = 0.f;
	if (len > 1e-6f) { nx = -dy / len; ny = dx / len; }

	// angle de l'axe A→B
	float base_angle = atan2f(dy, dx);

	// demi-cercle autour de B — face opposée à A
	// plage : [base_angle - PI/2 → base_angle + PI/2]
	for (int i = 0; i < half_seg; i++)
	{
		float a0 = base_angle - (float)M_PI / 2.f + (float)M_PI * (float)i       / (float)half_seg;
		float a1 = base_angle - (float)M_PI / 2.f + (float)M_PI * (float)(i + 1) / (float)half_seg;

		ctx_.debug_renderers[_sceneid].lines.emplace_back(b_x + radius * cosf(a0), b_y + radius * sinf(a0), b_z, r, g, b);
		ctx_.debug_renderers[_sceneid].lines.emplace_back(b_x + radius * cosf(a1), b_y + radius * sinf(a1), b_z, r, g, b);
	}

	// demi-cercle autour de A — face opposée à B
	// plage : [base_angle + PI/2 → base_angle + 3PI/2]
	for (int i = 0; i < half_seg; i++)
	{
		float a0 = base_angle + (float)M_PI / 2.f + (float)M_PI * (float)i       / (float)half_seg;
		float a1 = base_angle + (float)M_PI / 2.f + (float)M_PI * (float)(i + 1) / (float)half_seg;

		ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x + radius * cosf(a0), a_y + radius * sinf(a0), a_z, r, g, b);
		ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x + radius * cosf(a1), a_y + radius * sinf(a1), a_z, r, g, b);
	}

	// deux segments latéraux reliant les demi-cercles
	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x + nx * radius, a_y + ny * radius, a_z, r, g, b);
	ctx_.debug_renderers[_sceneid].lines.emplace_back(b_x + nx * radius, b_y + ny * radius, b_z, r, g, b);

	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x - nx * radius, a_y - ny * radius, a_z, r, g, b);
	ctx_.debug_renderers[_sceneid].lines.emplace_back(b_x - nx * radius, b_y - ny * radius, b_z, r, g, b);

}

void HRL_DrawDebugPoint(HRL_id _sceneid, float a_x, float a_y, float a_z, float size, float r, float g, float b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugPoint: invalid scene ID"); return; }

	float h = size * 0.5f;

	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x - h, a_y,     a_z, r, g, b);
	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x + h, a_y,     a_z, r, g, b);

	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x,     a_y - h, a_z, r, g, b);
	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x,     a_y + h, a_z, r, g, b);
}


void HRL_DrawSceneAsDebugMode(HRL_id _sceneid, HRL_EDebugView mode)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawSceneAsDebugMode: invalid scene ID");
		return;
	}

	switch (mode)
	{
	case HRL_DEBUG_VIEW_NONE:
	case HRL_DEBUG_VIEW_UNLIT:
	case HRL_DEBUG_VIEW_NORMAL:
	case HRL_DEBUG_VIEW_LIGHTS:
	case HRL_DEBUG_VIEW_WIREFRAME:
	case HRL_DEBUG_VIEW_LOD:
	case HRL_DEBUG_VIEW_MESH_INFO:
		it->second->debug_view = mode;
		break;
	default:
		SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_DrawSceneAsDebugMode: invalid debug view mode");
		break;
	}
}


void HRL_SetDebugMeshInfoFont(HRL_id _sceneid, HRL_id _fontid)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetDebugMeshInfoFont: invalid scene ID");
		return;
	}
	if (_fontid != HRL_INVALID_ID && ctx_.fonts.find(_fontid) == ctx_.fonts.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetDebugMeshInfoFont: invalid font ID");
		return;
	}
	it->second->debug_mesh_info_font = _fontid;
}

void HRL_SetDebugMeshInfoTextSize(HRL_id _sceneid, float _size)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetDebugMeshInfoTextSize: invalid scene ID");
		return;
	}
	if (!std::isfinite(_size) || _size <= 0.0f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetDebugMeshInfoTextSize: size must be finite and positive");
		return;
	}
	// Debug mesh info text size is expressed in screen pixels.
	// Keep the value as-is so the OpenGL debug pass can render it at the
	// requested pixel height independently of the viewport resolution.
	it->second->debug_mesh_info_text_size = std::clamp(_size, 1.0f, 256.0f);
}

void HRL_SetDebugMeshInfoTextColor(HRL_id _sceneid, float r, float g, float b, float a)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetDebugMeshInfoTextColor: invalid scene ID");
		return;
	}
	if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) || !std::isfinite(a))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetDebugMeshInfoTextColor: color values must be finite");
		return;
	}
	it->second->debug_mesh_info_text_color = glm::clamp(glm::vec4(r, g, b, a), glm::vec4(0.0f), glm::vec4(1.0f));
}

HRL_id HRL_AddScreenMessageText(HRL_id _sceneid, float _duration_seconds, const char* _text)
{
	if (!_text || !_text[0])
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_AddScreenMessageText: text must not be empty");
		return HRL_INVALID_ID;
	}
	return HRL_AddScreenMessage(_sceneid, _duration_seconds, "%s", _text);
}

HRL_id HRL_AddScreenMessage(HRL_id _sceneid, float _duration_seconds, const char* _format, ...)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_AddScreenMessage: invalid scene ID");
		return HRL_INVALID_ID;
	}
	if (!_format || !_format[0])
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_AddScreenMessage: format must not be empty");
		return HRL_INVALID_ID;
	}
	if (!std::isfinite(_duration_seconds) || _duration_seconds <= 0.0f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_AddScreenMessage: duration must be finite and positive");
		return HRL_INVALID_ID;
	}

	va_list args;
	va_start(args, _format);
	va_list args_copy;
	va_copy(args_copy, args);
	const int required = std::vsnprintf(nullptr, 0, _format, args_copy);
	va_end(args_copy);

	if (required < 0)
	{
		va_end(args);
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_AddScreenMessage: invalid format string");
		return HRL_INVALID_ID;
	}

	std::string formatted(static_cast<size_t>(required) + 1, '\0');
	std::vsnprintf(formatted.data(), formatted.size(), _format, args);
	formatted.resize(static_cast<size_t>(required));
	va_end(args);

	HRL_ScreenMessage message;
	message.id = it->second->next_screen_message_id++;
	if (message.id == HRL_INVALID_ID)
		message.id = it->second->next_screen_message_id++;
	message.text = std::move(formatted);
	message.remaining_seconds = _duration_seconds;
	message.size = it->second->screen_message_text_size;
	message.color = it->second->screen_message_text_color;
	message.font = it->second->screen_message_font;
	if (message.font == HRL_INVALID_ID)
		message.font = it->second->debug_mesh_info_font;

	it->second->screen_messages.push_back(std::move(message));
	return it->second->screen_messages.back().id;
}

void HRL_SetScreenMessageTextSize(HRL_id _sceneid, float _size)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenMessageTextSize: invalid scene ID");
		return;
	}
	if (!std::isfinite(_size) || _size <= 0.0f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetScreenMessageTextSize: size must be finite and positive");
		return;
	}
	it->second->screen_message_text_size = std::clamp(_size, 1.0f, 256.0f);
}

void HRL_SetScreenMessageTextColor(HRL_id _sceneid, float r, float g, float b, float a)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenMessageTextColor: invalid scene ID");
		return;
	}
	if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) || !std::isfinite(a))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetScreenMessageTextColor: color values must be finite");
		return;
	}
	it->second->screen_message_text_color = glm::clamp(glm::vec4(r, g, b, a), glm::vec4(0.0f), glm::vec4(1.0f));
}

void HRL_SetScreenMessageFont(HRL_id _sceneid, HRL_id _fontid)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenMessageFont: invalid scene ID");
		return;
	}
	if (_fontid != HRL_INVALID_ID && ctx_.fonts.find(_fontid) == ctx_.fonts.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenMessageFont: invalid font ID");
		return;
	}
	it->second->screen_message_font = _fontid;
}
