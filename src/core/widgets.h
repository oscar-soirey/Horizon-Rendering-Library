#pragma once

#include "hrl.h"
#include <glm/glm.hpp>

#include <array>
#include <string>
#include <vector>

class HRL_Widget {
public:
	HRL_Widget() = default;
	virtual ~HRL_Widget() = default;

	virtual void Logic() = 0;
	virtual bool IsPointerInteractive() const { return false; }

	void SetPosition(float x, float y);
	void SetWorldPosition(float x, float y, float z);
	void SetWorldPositionEnabled(bool enabled);
	bool IsWorldPositionEnabled() const { return world_position_enabled_; }
	const glm::vec3& GetWorldPosition() const { return world_position_; }
	const glm::vec2& GetPosition() const { return position_; }
	void SetScale(float x, float y);
	void SetAlpha(float a);
	void SetAnchor(float ax, float ay);
	void SetVisible(bool visible);
	void SetEnabled(bool enabled);
	void SetZIndex(int z);

	void SetViewport(HRL_id viewport);
	void SetId(HRL_id id) { id_ = id; }
	HRL_id GetViewport() const { return viewport_; }
	HRL_id GetId() const { return id_; }
	int GetZIndex() const { return z_index_; }

	// Layout helpers used by concrete widgets. Position remains normalized to
	// the viewport; the stored size is in pixels so it does not stretch on resize.
	float GetWidthPixels() const { return fixed_size_pixels_.x; }
	float GetHeightPixels() const { return fixed_size_pixels_.y; }
	float GetLeftNormalized() const { return position_.x - (fixed_size_pixels_.x * anchor_.x) / viewport_width_; }
	float GetTopNormalized() const { return position_.y - (fixed_size_pixels_.y * anchor_.y) / viewport_height_; }

	bool IsHovered() const { return hovered_; }
	bool IsVisible() const { return visible_; }
	bool IsEnabled() const { return enabled_; }

	void UpdateInput(float mouse_x, float mouse_y,
		bool left_down, bool left_pressed, bool left_released,
		float viewport_x, float viewport_y,
		float viewport_width, float viewport_height);

	struct WidgetDrawInfos {
		float px;
		float py;
		float sx;
		float sy;
		HRL_id texture;
		float r;
		float g;
		float b;
		float a;
		bool sdf = false;
	};

	virtual void GetDrawInfos(std::vector<WidgetDrawInfos>& infos) = 0;

protected:
	bool ContainsMouse() const;
	void ApplyAlpha(glm::vec4& color) const;

	// Position is normalized to the owning viewport (0..1).
	// Size is expressed through SetScale using the public normalized API, but is
	// captured as pixels for the current viewport so resizing never stretches a widget.
	glm::vec2 position_{0.0f};
	glm::vec2 scale_{0.1f, 0.05f};
	glm::vec2 anchor_{0.0f};
	glm::vec3 world_position_{0.0f};
	bool world_position_enabled_ = false;
	glm::vec2 fixed_size_pixels_{0.0f};
	bool fixed_size_initialized_ = false;

	float alpha_ = 1.0f;
	bool visible_ = true;
	bool enabled_ = true;
	bool hovered_ = false;

	HRL_id id_ = HRL_INVALID_ID;
	HRL_id viewport_ = HRL_INVALID_ID;
	int z_index_ = 0;

	float mouse_x_ = 0.0f;
	float mouse_y_ = 0.0f;
	float viewport_x_ = 0.0f;
	float viewport_y_ = 0.0f;
	float viewport_width_ = 1.0f;
	float viewport_height_ = 1.0f;
	bool left_down_ = false;
	bool left_pressed_ = false;
	bool left_released_ = false;
};

class HRL_WidgetButton : public HRL_Widget {
public:
	HRL_WidgetButton();
	~HRL_WidgetButton() override;

	void Logic() override;
	bool IsPointerInteractive() const override { return true; }
	void GetDrawInfos(std::vector<WidgetDrawInfos>& infos) override;

	void GenerateTextTexture();

	HRL_CButtonPressed pressed_callback = nullptr;
	void* pressed_user_data = nullptr;

	void SetText(const char* text);
	void SetFont(HRL_id font);
	void SetTextSize(float size);
	void SetTextTintColor(HRL_EWidgetState state, const glm::vec4& color);
	void SetBackgroundTexture(HRL_EWidgetState state, HRL_id texture);
	void SetBackgroundTintColor(HRL_EWidgetState state, const glm::vec4& color);
	void SetClickable(bool clickable);

public:
	std::array<HRL_id, 3> background_textures_{};
	std::array<glm::vec4, 3> background_tint_colors_{};

	HRL_id text_texture_ = HRL_INVALID_ID;
	std::array<glm::vec4, 3> text_tint_colors_{};
	float text_size_ = 15.0f;
	HRL_id font_ = HRL_INVALID_ID;
	std::string text_text_;

private:
	bool pressed_last_frame_ = false;
	bool click_capture_ = false;
	bool clickable_ = true;
};

class HRL_WidgetLabel : public HRL_Widget {
public:
	HRL_WidgetLabel() = default;
	~HRL_WidgetLabel() override;

	void Logic() override {}
	void GetDrawInfos(std::vector<WidgetDrawInfos>& infos) override;
	void GenerateTextTexture();

	void SetText(const char* text);
	void SetFont(HRL_id font);
	void SetTextSize(float size);
	void SetTintColor(const glm::vec4& color);

private:
	HRL_id text_texture_ = HRL_INVALID_ID;
	HRL_id font_ = HRL_INVALID_ID;
	std::string text_text_;
	float text_size_ = 15.0f;
	glm::vec4 tint_color_{1.0f};
};

class HRL_WidgetImage : public HRL_Widget {
public:
	void Logic() override {}
	void GetDrawInfos(std::vector<WidgetDrawInfos>& infos) override;

	HRL_id texture_ = HRL_INVALID_ID;
	glm::vec4 tint_color_{1.0f};
};

class HRL_WidgetSlider : public HRL_Widget {
public:
	void Logic() override;
	bool IsPointerInteractive() const override { return true; }
	void GetDrawInfos(std::vector<WidgetDrawInfos>& infos) override;

	float value_ = 0.0f;
	float minimum_ = 0.0f;
	float maximum_ = 1.0f;
	HRL_ESliderOrientation orientation_ = HRL_SLIDER_HORIZONTAL;
	bool clickable_ = true;
	bool dragging_ = false;
	glm::vec4 background_color_{0.18f, 0.18f, 0.18f, 1.0f};
	glm::vec4 fill_color_{0.30f, 0.65f, 1.0f, 1.0f};
	glm::vec4 handle_color_{0.85f, 0.85f, 0.85f, 1.0f};
	HRL_CSliderChanged changed_callback = nullptr;
	void* changed_user_data = nullptr;

	void SetValue(float value);
};

class HRL_WidgetCheckbox : public HRL_Widget {
public:
	void Logic() override;
	bool IsPointerInteractive() const override { return true; }
	void GetDrawInfos(std::vector<WidgetDrawInfos>& infos) override;

	bool checked_ = false;
	bool clickable_ = true;
	bool click_capture_ = false;
	glm::vec4 background_color_{0.18f, 0.18f, 0.18f, 1.0f};
	glm::vec4 checked_color_{0.30f, 0.65f, 1.0f, 1.0f};
	HRL_CCheckboxChanged changed_callback = nullptr;
	void* changed_user_data = nullptr;

	void SetChecked(bool checked);
};

class HRL_WidgetProgressBar : public HRL_Widget {
public:
	void Logic() override {}
	void GetDrawInfos(std::vector<WidgetDrawInfos>& infos) override;

	float value_ = 0.0f;
	glm::vec4 background_color_{0.18f, 0.18f, 0.18f, 1.0f};
	glm::vec4 fill_color_{0.30f, 0.65f, 1.0f, 1.0f};
	void SetValue(float value);
};
