#include "widgets.h"

#include "utils_functions.h"

#include <algorithm>
#include <cmath>

static int WidgetStateIndex(HRL_EWidgetState state)
{
	switch (state)
	{
	case HRL_WIDGET_STATE_HOVERED: return 1;
	case HRL_WIDGET_STATE_PRESSED: return 2;
	case HRL_WIDGET_STATE_IDLE:
	default: return 0;
	}
}

static int ClampWidgetState(int state)
{
	return std::clamp(state, 0, 2);
}

// WIDGET BASE
void HRL_Widget::SetPosition(float x, float y)
{
	position_ = {x, y};
}

void HRL_Widget::SetWorldPosition(float x, float y, float z)
{
	world_position_ = {x, y, z};
	world_position_enabled_ = true;
}

void HRL_Widget::SetWorldPositionEnabled(bool enabled)
{
	world_position_enabled_ = enabled;
}

void HRL_Widget::SetScale(float x, float y)
{
	scale_ = {std::max(0.0f, x), std::max(0.0f, y)};
	// Keep the existing API and its normalized arguments, but recapture the
	// corresponding pixel size on the next frame. Once captured, the size stays
	// constant while the window is resized; only the anchor/position is recomputed.
	fixed_size_initialized_ = false;
}

void HRL_Widget::SetAlpha(float a)
{
	alpha_ = std::clamp(a, 0.0f, 1.0f);
}

void HRL_Widget::SetAnchor(float ax, float ay)
{
	anchor_ = {std::clamp(ax, 0.0f, 1.0f), std::clamp(ay, 0.0f, 1.0f)};
}

void HRL_Widget::SetVisible(bool visible)
{
	visible_ = visible;
	if (!visible_)
		hovered_ = false;
}

void HRL_Widget::SetEnabled(bool enabled)
{
	enabled_ = enabled;
}

void HRL_Widget::SetZIndex(int z)
{
	z_index_ = z;
}

void HRL_Widget::SetViewport(HRL_id viewport)
{
	viewport_ = viewport;
	fixed_size_initialized_ = false;
}

bool HRL_Widget::ContainsMouse() const
{
	if (world_position_enabled_)
		return false;

	const float width = GetWidthPixels();
	const float height = GetHeightPixels();
	const float left = viewport_x_ + position_.x * viewport_width_ - width * anchor_.x;
	const float top = viewport_y_ + position_.y * viewport_height_ - height * anchor_.y;
	const float right = left + width;
	const float bottom = top + height;
	return mouse_x_ >= left && mouse_x_ <= right && mouse_y_ >= top && mouse_y_ <= bottom;
}

void HRL_Widget::UpdateInput(float mouse_x, float mouse_y,
	bool left_down, bool left_pressed, bool left_released,
	float viewport_x, float viewport_y,
	float viewport_width, float viewport_height)
{
	mouse_x_ = mouse_x;
	mouse_y_ = mouse_y;
	left_down_ = left_down;
	left_pressed_ = left_pressed;
	left_released_ = left_released;
	viewport_x_ = viewport_x;
	viewport_y_ = viewport_y;
	viewport_width_ = std::max(1.0f, viewport_width);
	viewport_height_ = std::max(1.0f, viewport_height);

	if (!fixed_size_initialized_)
	{
		// Preserve the exact visual size requested by the old normalized API at
		// the moment the widget is first laid out. The resulting pixel size is
		// intentionally independent from later viewport aspect-ratio changes.
		fixed_size_pixels_.x = scale_.x * viewport_width_;
		fixed_size_pixels_.y = scale_.y * viewport_height_;
		fixed_size_initialized_ = true;
	}

	hovered_ = visible_ && ContainsMouse();
}

void HRL_Widget::ApplyAlpha(glm::vec4& color) const
{
	color.w *= alpha_;
}

// BUTTON
HRL_WidgetButton::HRL_WidgetButton()
{
	background_textures_.fill(HRL_INVALID_ID);
	background_tint_colors_.fill(glm::vec4(1.0f));
	text_tint_colors_.fill(glm::vec4(1.0f));
}

HRL_WidgetButton::~HRL_WidgetButton()
{
	if (HRL_IsValidTexture(text_texture_))
		HRL_DeleteTexture(text_texture_);
	text_texture_ = HRL_INVALID_ID;
}

void HRL_WidgetButton::SetText(const char* text)
{
	text_text_ = text ? text : "";
	GenerateTextTexture();
}

void HRL_WidgetButton::SetFont(HRL_id font)
{
	font_ = font;
	GenerateTextTexture();
}

void HRL_WidgetButton::SetTextSize(float size)
{
	text_size_ = std::max(1.0f, size);
	GenerateTextTexture();
}

void HRL_WidgetButton::SetTextTintColor(HRL_EWidgetState state, const glm::vec4& color)
{
	text_tint_colors_[ClampWidgetState(WidgetStateIndex(state))] = color;
}

void HRL_WidgetButton::SetBackgroundTexture(HRL_EWidgetState state, HRL_id texture)
{
	background_textures_[ClampWidgetState(WidgetStateIndex(state))] = texture;
}

void HRL_WidgetButton::SetBackgroundTintColor(HRL_EWidgetState state, const glm::vec4& color)
{
	background_tint_colors_[ClampWidgetState(WidgetStateIndex(state))] = color;
}

void HRL_WidgetButton::SetClickable(bool clickable)
{
	clickable_ = clickable;
	if (!clickable_)
		click_capture_ = false;
}

void HRL_WidgetButton::Logic()
{
	const bool active = visible_ && enabled_ && clickable_;
	const bool inside = visible_ && ContainsMouse();

	if (active && left_pressed_ && inside)
		click_capture_ = true;

	if (!left_down_ && !left_released_)
		pressed_last_frame_ = false;

	if (active && click_capture_ && left_down_)
		pressed_last_frame_ = true;

	const bool clicked = active && left_pressed_ && inside;
	const bool released = active && click_capture_ && left_released_;
	const bool held = active && click_capture_ && left_down_;

	if (pressed_callback && (clicked || released || held))
		pressed_callback(id_, clicked ? HRL_TRUE : HRL_FALSE, released ? HRL_TRUE : HRL_FALSE, pressed_user_data);

	if (left_released_)
		click_capture_ = false;
}

void HRL_WidgetButton::GetDrawInfos(std::vector<WidgetDrawInfos>& infos)
{
	if (!visible_)
		return;

	const int state = (enabled_ && click_capture_ && left_down_) ? 2 : ((enabled_ && hovered_) ? 1 : 0);
	glm::vec4 background = background_tint_colors_[state];
	ApplyAlpha(background);
	const float width = GetWidthPixels();
	const float height = GetHeightPixels();
	const float left_px = position_.x * viewport_width_ - width * anchor_.x;
	const float top_px = position_.y * viewport_height_ - height * anchor_.y;
	const float left = left_px / viewport_width_;
	const float top = top_px / viewport_height_;
	const float sx = width / viewport_width_;
	const float sy = height / viewport_height_;
	infos.push_back({
		left, top, sx, sy,
		background_textures_[state],
		background.r, background.g, background.b, background.w
	});

	if (text_texture_ == HRL_INVALID_ID || !HRL_IsValidTexture(text_texture_))
		return;

	int texture_width = 0;
	int texture_height = 0;
	HRL_GetTextureSize(text_texture_, &texture_width, &texture_height);
	if (texture_width <= 0 || texture_height <= 0)
		return;

	const float available_width_px = std::max(1.0f, GetWidthPixels());
	const float available_height_px = std::max(1.0f, GetHeightPixels());
	const float texture_aspect = static_cast<float>(texture_width) / static_cast<float>(texture_height);
	float text_width_px = std::min(available_width_px, available_height_px * texture_aspect);
	float text_height_px = text_width_px / texture_aspect;
	if (text_height_px > available_height_px)
	{
		text_height_px = available_height_px;
		text_width_px = text_height_px * texture_aspect;
	}

	const float text_sx = text_width_px / viewport_width_;
	const float text_sy = text_height_px / viewport_height_;
	const float widget_left = (position_.x * viewport_width_ - GetWidthPixels() * anchor_.x) / viewport_width_;
	const float widget_top = (position_.y * viewport_height_ - GetHeightPixels() * anchor_.y) / viewport_height_;
	const float text_px = widget_left + (width / viewport_width_ - text_sx) * 0.5f;
	const float text_py = widget_top + (height / viewport_height_ - text_sy) * 0.5f;

	glm::vec4 text_color = text_tint_colors_[state];
	ApplyAlpha(text_color);
	infos.push_back({text_px, text_py, text_sx, text_sy, text_texture_,
		text_color.r, text_color.g, text_color.b, text_color.a, true});
}

void HRL_WidgetButton::GenerateTextTexture()
{
	if (HRL_IsValidTexture(text_texture_))
		HRL_DeleteTexture(text_texture_);
	text_texture_ = HRL_INVALID_ID;

	if (text_text_.empty() || font_ == HRL_INVALID_ID || !HRL_IsValidFont(font_))
		return;

	// Text is generated white and recolored by the widget state tint.
	text_texture_ = HRL_InternalCreateSDFTextTexture(text_text_.c_str(), font_, text_size_);
}

// LABEL
HRL_WidgetLabel::~HRL_WidgetLabel()
{
	if (HRL_IsValidTexture(text_texture_))
		HRL_DeleteTexture(text_texture_);
	text_texture_ = HRL_INVALID_ID;
}

void HRL_WidgetLabel::SetText(const char* text)
{
	text_text_ = text ? text : "";
	GenerateTextTexture();
}

void HRL_WidgetLabel::SetFont(HRL_id font)
{
	font_ = font;
	GenerateTextTexture();
}

void HRL_WidgetLabel::SetTextSize(float size)
{
	text_size_ = std::max(1.0f, size);
	GenerateTextTexture();
}

void HRL_WidgetLabel::SetTintColor(const glm::vec4& color)
{
	tint_color_ = color;
}

void HRL_WidgetLabel::GenerateTextTexture()
{
	if (HRL_IsValidTexture(text_texture_))
		HRL_DeleteTexture(text_texture_);
	text_texture_ = HRL_INVALID_ID;

	if (text_text_.empty() || font_ == HRL_INVALID_ID || !HRL_IsValidFont(font_))
		return;

	text_texture_ = HRL_InternalCreateSDFTextTexture(text_text_.c_str(), font_, text_size_);
}

void HRL_WidgetLabel::GetDrawInfos(std::vector<WidgetDrawInfos>& infos)
{
	if (!visible_ || text_texture_ == HRL_INVALID_ID || !HRL_IsValidTexture(text_texture_))
		return;

	int texture_width = 0;
	int texture_height = 0;
	HRL_GetTextureSize(text_texture_, &texture_width, &texture_height);
	if (texture_width <= 0 || texture_height <= 0)
		return;

	const float available_width_px = std::max(1.0f, GetWidthPixels());
	const float available_height_px = std::max(1.0f, GetHeightPixels());
	const float texture_aspect = static_cast<float>(texture_width) / static_cast<float>(texture_height);
	float text_width_px = std::min(available_width_px, available_height_px * texture_aspect);
	float text_height_px = text_width_px / texture_aspect;
	if (text_height_px > available_height_px)
	{
		text_height_px = available_height_px;
		text_width_px = text_height_px * texture_aspect;
	}

	const float text_sx = text_width_px / viewport_width_;
	const float text_sy = text_height_px / viewport_height_;
	const float widget_left = (position_.x * viewport_width_ - GetWidthPixels() * anchor_.x) / viewport_width_;
	const float widget_top = (position_.y * viewport_height_ - GetHeightPixels() * anchor_.y) / viewport_height_;
	glm::vec4 color = tint_color_;
	ApplyAlpha(color);
	infos.push_back({
		widget_left + (GetWidthPixels() / viewport_width_ - text_sx) * 0.5f,
		widget_top + (GetHeightPixels() / viewport_height_ - text_sy) * 0.5f,
		text_sx, text_sy, text_texture_,
		color.r, color.g, color.b, color.a, true});
}

// IMAGE
void HRL_WidgetImage::GetDrawInfos(std::vector<WidgetDrawInfos>& infos)
{
	if (!visible_)
		return;
	glm::vec4 color = tint_color_;
	ApplyAlpha(color);
	const float width = GetWidthPixels();
	const float height = GetHeightPixels();
	const float left_px = position_.x * viewport_width_ - width * anchor_.x;
	const float top_px = position_.y * viewport_height_ - height * anchor_.y;
	const float left = left_px / viewport_width_;
	const float top = top_px / viewport_height_;
	const float sx = width / viewport_width_;
	const float sy = height / viewport_height_;
	infos.push_back({
		left, top, sx, sy,
		texture_, color.r, color.g, color.b, color.a});
}

// SLIDER
void HRL_WidgetSlider::SetValue(float value)
{
	if (maximum_ < minimum_)
		std::swap(minimum_, maximum_);
	value_ = std::clamp(value, minimum_, maximum_);
}

void HRL_WidgetSlider::Logic()
{
	if (!visible_ || !enabled_ || !clickable_)
	{
		dragging_ = false;
		return;
	}

	if (left_pressed_ && hovered_)
		dragging_ = true;

	if (dragging_ && left_down_)
	{
		float t = 0.0f;
		if (orientation_ == HRL_SLIDER_VERTICAL)
		{
			const float top = viewport_y_ + position_.y * viewport_height_ - GetHeightPixels() * anchor_.y;
			t = 1.0f - (mouse_y_ - top) / std::max(1.0f, GetHeightPixels());
		}
		else
		{
			const float left = viewport_x_ + position_.x * viewport_width_ - GetWidthPixels() * anchor_.x;
			t = (mouse_x_ - left) / std::max(1.0f, GetWidthPixels());
		}
		t = std::clamp(t, 0.0f, 1.0f);
		const float old_value = value_;
		value_ = minimum_ + (maximum_ - minimum_) * t;
		if (changed_callback && old_value != value_)
			changed_callback(id_, value_, changed_user_data);
	}

	if (left_released_)
		dragging_ = false;
}

void HRL_WidgetSlider::GetDrawInfos(std::vector<WidgetDrawInfos>& infos)
{
	if (!visible_)
		return;

	const float range = maximum_ - minimum_;
	const float t = (range == 0.0f) ? 0.0f : std::clamp((value_ - minimum_) / range, 0.0f, 1.0f);
	const float left = GetLeftNormalized();
	const float top = GetTopNormalized();
	const float width = GetWidthPixels() / viewport_width_;
	const float height = GetHeightPixels() / viewport_height_;

	glm::vec4 bg = background_color_;
	glm::vec4 fill = fill_color_;
	glm::vec4 handle = handle_color_;
	ApplyAlpha(bg); ApplyAlpha(fill); ApplyAlpha(handle);

	infos.push_back({left, top, width, height, HRL_INVALID_ID, bg.r, bg.g, bg.b, bg.a});
	if (orientation_ == HRL_SLIDER_VERTICAL)
	{
		const float filled = height * t;
		infos.push_back({left, top + height - filled, width, filled, HRL_INVALID_ID, fill.r, fill.g, fill.b, fill.a});
		const float handle_size_px = std::min(GetWidthPixels(), GetHeightPixels()) * 0.18f;
		const float handle_size_x = handle_size_px / viewport_width_;
		const float handle_size_y = handle_size_px / viewport_height_;
		const float handle_y = top + height - height * t - handle_size_y * 0.5f;
		infos.push_back({left + (width - handle_size_x) * 0.5f, handle_y,
			handle_size_x, handle_size_y, HRL_INVALID_ID, handle.r, handle.g, handle.b, handle.a});
	}
	else
	{
		const float filled = width * t;
		infos.push_back({left, top, filled, height, HRL_INVALID_ID, fill.r, fill.g, fill.b, fill.a});
		const float handle_size_px = std::min(GetWidthPixels(), GetHeightPixels()) * 0.18f;
		const float handle_size_x = handle_size_px / viewport_width_;
		const float handle_size_y = handle_size_px / viewport_height_;
		const float handle_x = left + width * t - handle_size_x * 0.5f;
		infos.push_back({handle_x, top + (height - handle_size_y) * 0.5f,
			handle_size_x, handle_size_y, HRL_INVALID_ID, handle.r, handle.g, handle.b, handle.a});
	}
}

// CHECKBOX
void HRL_WidgetCheckbox::SetChecked(bool checked)
{
	if (checked_ == checked)
		return;
	checked_ = checked;
	if (changed_callback)
		changed_callback(id_, checked_ ? HRL_TRUE : HRL_FALSE, changed_user_data);
}

void HRL_WidgetCheckbox::Logic()
{
	if (!visible_ || !enabled_ || !clickable_)
		return;

	if (left_pressed_ && hovered_)
		click_capture_ = true;

	if (left_released_ && click_capture_)
	{
		if (hovered_)
			SetChecked(!checked_);
		click_capture_ = false;
	}
}

void HRL_WidgetCheckbox::GetDrawInfos(std::vector<WidgetDrawInfos>& infos)
{
	if (!visible_)
		return;

	const float left = GetLeftNormalized();
	const float top = GetTopNormalized();
	glm::vec4 bg = background_color_;
	glm::vec4 checked = checked_color_;
	ApplyAlpha(bg); ApplyAlpha(checked);
	const float width = GetWidthPixels() / viewport_width_;
	const float height = GetHeightPixels() / viewport_height_;
	infos.push_back({left, top, width, height, HRL_INVALID_ID, bg.r, bg.g, bg.b, bg.a});

	if (checked_)
	{
		const float padding_x = width * 0.22f;
		const float padding_y = height * 0.22f;
		infos.push_back({left + padding_x, top + padding_y,
			width - padding_x * 2.0f, height - padding_y * 2.0f,
			HRL_INVALID_ID, checked.r, checked.g, checked.b, checked.a});
	}
}

// PROGRESS BAR
void HRL_WidgetProgressBar::SetValue(float value)
{
	value_ = std::clamp(value, 0.0f, 1.0f);
}

void HRL_WidgetProgressBar::GetDrawInfos(std::vector<WidgetDrawInfos>& infos)
{
	if (!visible_)
		return;

	const float left = GetLeftNormalized();
	const float top = GetTopNormalized();
	const float width = GetWidthPixels() / viewport_width_;
	const float height = GetHeightPixels() / viewport_height_;
	glm::vec4 bg = background_color_;
	glm::vec4 fill = fill_color_;
	ApplyAlpha(bg); ApplyAlpha(fill);
	infos.push_back({left, top, width, height, HRL_INVALID_ID, bg.r, bg.g, bg.b, bg.a});
	infos.push_back({left, top, width * value_, height, HRL_INVALID_ID, fill.r, fill.g, fill.b, fill.a});
}
