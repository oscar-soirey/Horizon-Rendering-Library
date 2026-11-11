// 2D overlays of the viewports for the backends isolated from the frontend
// state (Vulkan): widgets, on-screen messages and the mesh information texts
// of HRL_DEBUG_VIEW_MESH_INFO. The frontend runs the widget logic (mouse
// input, capture, callbacks) and produces textured quads; the backend only
// draws them (HRL_SceneFrame::overlays). The OpenGL backends keep doing this
// work themselves and never call these functions.
#include "frontend_internal.h"
#include "../core/render_common.h"

#include <sstream>

namespace
{
// SDF text textures generated for the overlays, kept while their text is shown.
struct TextTexture
{
	HRL_id texture = HRL_INVALID_ID;
	HRL_id font = HRL_INVALID_ID;
	float size = 0.f;
	std::string text;
	int width = 0, height = 0;
	bool used = false;
};
std::unordered_map<HRL_id, TextTexture> g_meshInfoTexts;  // by mesh id
std::unordered_map<HRL_id, TextTexture> g_messageTexts;   // by message id

void ReleaseText(TextTexture& t)
{
	if (t.texture != HRL_INVALID_ID && g_Backend.RHI_IsValidTexture && g_Backend.RHI_IsValidTexture(t.texture))
		g_Backend.RHI_DeleteTexture(t.texture);
	t = TextTexture{};
}

// Regenerates the texture when the text, the font or the size changed.
bool EnsureText(TextTexture& t, const std::string& text, HRL_id font, float size)
{
	t.used = true;
	const bool valid = t.texture != HRL_INVALID_ID && g_Backend.RHI_IsValidTexture && g_Backend.RHI_IsValidTexture(t.texture);
	if (valid && t.font == font && t.size == size && t.text == text) return t.width > 0 && t.height > 0;
	ReleaseText(t);
	t.used = true;
	t.font = font;
	t.size = size;
	t.text = text;
	t.texture = HRL_InternalCreateSDFTextTexture(text.c_str(), font, size);
	if (t.texture == HRL_INVALID_ID) return false;
	g_Backend.RHI_SetTextureMinFilter(t.texture, HRL_FILTER_LINEAR);
	g_Backend.RHI_SetTextureMaxFilter(t.texture, HRL_FILTER_LINEAR);
	g_Backend.RHI_GetTextureSize(t.texture, &t.width, &t.height);
	return t.width > 0 && t.height > 0;
}

// Deletes the textures of the texts not shown this frame.
void SweepTexts(std::unordered_map<HRL_id, TextTexture>& cache)
{
	for (auto it = cache.begin(); it != cache.end();)
	{
		if (!it->second.used)
		{
			ReleaseText(it->second);
			it = cache.erase(it);
		}
		else
		{
			it->second.used = false;
			++it;
		}
	}
}

HRL_id ResolveFont(HRL_id preferred, HRL_id fallback)
{
	if (preferred != HRL_INVALID_ID && ctx_.fonts.find(preferred) != ctx_.fonts.end()) return preferred;
	if (fallback != HRL_INVALID_ID && ctx_.fonts.find(fallback) != ctx_.fonts.end()) return fallback;
	return ctx_.fonts.empty() ? HRL_INVALID_ID : ctx_.fonts.begin()->first;
}

// Camera of a viewport in OpenGL clip space, with the window as reference
// size (the convention of the widget and gizmo input).
glm::mat4 ViewportViewProj(const HRL_Viewport* viewport, float pixelWidth, float pixelHeight)
{
	const HRL_Camera* cam = viewport->camera_;
	const glm::mat4 view = glm::lookAt(cam->position_, cam->position_ + GetForwardVector(cam->rotation_), GetUpVector(cam->rotation_));
	const float aspect = pixelWidth / std::max(pixelHeight, 1.f);
	const float nearPlane = std::max(cam->near_plane_, 1e-4f);
	const float farPlane = std::max(cam->far_plane_, nearPlane + 1e-3f);
	glm::mat4 proj;
	if (cam->type_ == HRL_PERSPECTIVE)
		proj = glm::perspective(glm::radians(glm::clamp(cam->value_, 1.f, 179.f)), aspect, nearPlane, farPlane);
	else
	{
		const float hh = cam->value_ * 0.5f, hw = hh * aspect;
		proj = glm::ortho(-hw, hw, -hh, hh, nearPlane, farPlane);
	}
	return proj * view;
}

bool ProjectToViewport(const glm::mat4& viewProj, const glm::vec3& world, glm::vec2& ndc)
{
	const glm::vec4 clip = viewProj * glm::vec4(world, 1.f);
	if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.w) || clip.w <= 1e-6f) return false;
	ndc = glm::vec2(clip) / clip.w;
	return std::isfinite(ndc.x) && std::isfinite(ndc.y);
}

void PushQuad(std::vector<HRL_OverlayQuad>& out, float x, float y, float w, float h,
	HRL_id texture, const glm::vec4& color, bool sdf)
{
	HRL_OverlayQuad q;
	q.x = x; q.y = y; q.w = w; q.h = h;
	q.texture = texture;
	q.color = color;
	q.sdf = sdf;
	out.push_back(q);
}

// HRL_DEBUG_VIEW_MESH_INFO: a text above the centre of every 3D mesh.
void MeshInfoTexts(const hrl_scene_t* scene, const HRL_Viewport* viewport, const glm::mat4& viewProj,
	float vpW, float vpH, std::vector<HRL_OverlayQuad>& out)
{
	const HRL_id font = ResolveFont(scene->debug_mesh_info_font, HRL_INVALID_ID);
	if (font == HRL_INVALID_ID) return;
	(void)viewport;
	const float textSize = scene->debug_mesh_info_text_size;
	for (const auto& [meshId, mesh] : scene->meshes)
	{
		if (!mesh || (mesh->type_ != HRL_3D_MESH && mesh->type_ != HRL_3D_SKELETAL_MESH)) continue;
		glm::vec2 ndc;
		const glm::vec3 center = glm::vec3(CalculateModelMatrix(mesh) * glm::vec4(mesh->bounds_center_, 1.f));
		if (!ProjectToViewport(viewProj, center, ndc)) continue;
		if (ndc.x < -1.15f || ndc.x > 1.15f || ndc.y < -1.15f || ndc.y > 1.15f) continue;
		std::string info;
		BuildMeshDebugInfoText(mesh, info);
		std::ostringstream withId;
		withId << "ID: " << static_cast<unsigned long long>(meshId) << "\n" << info;
		info = withId.str();
		TextTexture& text = g_meshInfoTexts[meshId];
		if (!EnsureText(text, info, font, textSize)) continue;
		// The text size is the height of one line.
		const float lines = 1.f + static_cast<float>(std::count(info.begin(), info.end(), '\n'));
		const float pixelHeight = std::max(1.f, textSize) * lines;
		const float w = pixelHeight * static_cast<float>(text.width) / static_cast<float>(text.height) / vpW;
		const float h = pixelHeight / vpH;
		const float ax = 0.5f + 0.5f * ndc.x, ay = 0.5f - 0.5f * ndc.y;
		PushQuad(out, ax - w * 0.5f, ay - h - 4.f / vpH, w, h, text.texture, scene->debug_mesh_info_text_color, true);
	}
}

// HRL_AddScreenMessageText: a stack of messages from the top-left corner.
void ScreenMessages(const hrl_scene_t* scene, float vpW, float vpH, std::vector<HRL_OverlayQuad>& out)
{
	float cursorY = 16.f;
	const float leftMargin = 16.f, lineGap = 4.f;
	for (const HRL_ScreenMessage& message : scene->screen_messages)
	{
		const HRL_id font = ResolveFont(message.font, scene->debug_mesh_info_font);
		if (font == HRL_INVALID_ID) return;
		TextTexture& text = g_messageTexts[message.id];
		if (!EnsureText(text, message.text, font, message.size)) continue;
		const float lines = 1.f + static_cast<float>(std::count(message.text.begin(), message.text.end(), '\n'));
		const float pixelHeight = std::max(1.f, message.size) * lines;
		const float pixelWidth = pixelHeight * static_cast<float>(text.width) / static_cast<float>(text.height);
		const float x = leftMargin / vpW, y = cursorY / vpH, w = pixelWidth / vpW, h = pixelHeight / vpH;
		// Dark shadow for readability, then the message in its colour.
		PushQuad(out, x + 1.5f / vpW, y + 1.5f / vpH, w, h, text.texture, glm::vec4(0.f, 0.f, 0.f, message.color.a * 0.75f), true);
		PushQuad(out, x, y, w, h, text.texture, message.color, true);
		cursorY += pixelHeight + lineGap;
	}
}

// Same input rules as the OpenGL backend: hover first, then the press goes to
// the interactive widget with the highest z under the cursor.
void Widgets(const HRL_Viewport* viewport, const glm::mat4& viewProj, std::vector<HRL_OverlayQuad>& out)
{
	if (viewport->widgets.empty()) return;
	const float winW = static_cast<float>(ctx_.window_width), winH = static_cast<float>(ctx_.window_height);
	const float vpX = static_cast<float>(static_cast<int>(viewport->x_ * winW));
	const float vpY = static_cast<float>(static_cast<int>(viewport->y_ * winH));
	const float vpW = static_cast<float>(std::max(1, static_cast<int>(viewport->width_ * winW)));
	const float vpH = static_cast<float>(std::max(1, static_cast<int>(viewport->height_ * winH)));

	std::vector<HRL_Widget*> sorted;
	sorted.reserve(viewport->widgets.size());
	for (const auto& [id, widget] : viewport->widgets)
		if (widget) sorted.push_back(widget);
	std::sort(sorted.begin(), sorted.end(), [](const HRL_Widget* a, const HRL_Widget* b)
	{
		if (a->GetZIndex() != b->GetZIndex()) return a->GetZIndex() < b->GetZIndex();
		return a->GetId() < b->GetId();
	});

	for (HRL_Widget* w : sorted)
		w->UpdateInput(ctx_.mouseX, ctx_.mouseY, false, false, false, vpX, vpY, vpW, vpH);
	if (ctx_.mouseLeftPressed && ctx_.mouseCaptureWidget == HRL_INVALID_ID && ctx_.mouseCaptureGizmo == HRL_INVALID_ID)
		for (auto it = sorted.rbegin(); it != sorted.rend(); ++it)
		{
			HRL_Widget* w = *it;
			if (w->IsPointerInteractive() && w->IsVisible() && w->IsEnabled() && w->IsHovered())
			{
				ctx_.mouseCaptureWidget = w->GetId();
				break;
			}
		}

	std::vector<HRL_Widget::WidgetDrawInfos> geometries;
	for (HRL_Widget* w : sorted)
	{
		const bool capture = w->GetId() == ctx_.mouseCaptureWidget;
		w->UpdateInput(ctx_.mouseX, ctx_.mouseY,
			capture && ctx_.mouseLeftDown, capture && ctx_.mouseLeftPressed, capture && ctx_.mouseLeftReleased,
			vpX, vpY, vpW, vpH);
		w->Logic();
		geometries.clear();
		w->GetDrawInfos(geometries);

		// A widget placed in the world keeps its 2D layout around the projected point.
		glm::vec2 delta(0.f);
		if (w->IsWorldPositionEnabled())
		{
			glm::vec2 ndc;
			if (!ProjectToViewport(viewProj, w->GetWorldPosition(), ndc)) continue;
			delta = glm::vec2(0.5f + 0.5f * ndc.x, 0.5f - 0.5f * ndc.y) - w->GetPosition();
		}
		for (const auto& g : geometries)
		{
			if (g.sx <= 0.f || g.sy <= 0.f || g.a <= 0.f) continue;
			HRL_OverlayQuad q;
			q.x = g.px + delta.x;
			q.y = g.py + delta.y;
			q.w = g.sx;
			q.h = g.sy;
			// t0 / t1 are top-down; the top of the quad shows v = 1 - t0.
			q.u0 = g.u0; q.v0 = 1.f - g.t0;
			q.u1 = g.u1; q.v1 = 1.f - g.t1;
			q.color = glm::vec4(g.r, g.g, g.b, g.a);
			q.texture = g.texture;
			q.sdf = g.sdf;
			out.push_back(q);
		}
	}
	if (ctx_.mouseLeftReleased) ctx_.mouseCaptureWidget = HRL_INVALID_ID;
}
} // namespace

void BuildSceneOverlays(const hrl_scene_t* scene, std::vector<HRL_ViewportOverlay>& out)
{
	out.clear();
	const float winW = static_cast<float>(std::max(1u, ctx_.window_width));
	const float winH = static_cast<float>(std::max(1u, ctx_.window_height));
	for (const auto& [viewportId, viewport] : scene->viewports)
	{
		if (!viewport || !viewport->camera_) continue;
		const float vpW = std::max(1.f, std::floor(viewport->width_ * winW));
		const float vpH = std::max(1.f, std::floor(viewport->height_ * winH));
		const glm::mat4 viewProj = ViewportViewProj(viewport, vpW, vpH);
		HRL_ViewportOverlay overlay;
		overlay.viewport = viewportId;
		if (scene->debug_view == HRL_DEBUG_VIEW_MESH_INFO)
			MeshInfoTexts(scene, viewport, viewProj, vpW, vpH, overlay.quads);
		ScreenMessages(scene, vpW, vpH, overlay.quads);
		Widgets(viewport, viewProj, overlay.quads);
		if (!overlay.quads.empty()) out.push_back(std::move(overlay));
	}
}

// Called once per frame after every scene was rendered.
void EndOverlayFrame()
{
	SweepTexts(g_meshInfoTexts);
	SweepTexts(g_messageTexts);
}

void ReleaseOverlays()
{
	// The backend frees every texture at shutdown: only forget them.
	g_meshInfoTexts.clear();
	g_messageTexts.clear();
}
