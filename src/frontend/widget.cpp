// HRL frontend - UI widgets and input.
#include "frontend_internal.h"

static HRL_Widget* FindWidget(HRL_id widget)
{
	auto it = ctx_.widgets.find(widget);
	return it == ctx_.widgets.end() ? nullptr : it->second;
}

static const HRL_Widget* FindWidgetConst(HRL_id widget)
{
	auto it = ctx_.widgets.find(widget);
	return it == ctx_.widgets.end() ? nullptr : it->second;
}

static int WidgetStateIndex(HRL_EWidgetState state)
{
	switch (state)
	{
	case HRL_WIDGET_STATE_IDLE: return 0;
	case HRL_WIDGET_STATE_HOVERED: return 1;
	case HRL_WIDGET_STATE_PRESSED: return 2;
	default: return -1;
	}
}

static void SetWidgetTypeError(const char* function, HRL_EWidgetType expected, HRL_id widget)
{
	(void)expected;
	(void)widget;
	SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, std::string(function) + ": invalid widget type");
}

template <typename T>
static T* FindTypedWidget(HRL_id widget, const char* function)
{
	HRL_Widget* base = FindWidget(widget);
	if (!base)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, std::string(function) + ": invalid widget ID");
		return nullptr;
	}
	T* typed = dynamic_cast<T*>(base);
	if (!typed)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, std::string(function) + ": invalid widget type");
		return nullptr;
	}
	return typed;
}

void HRL_MouseMovedCallback(float x, float y)
{
	ctx_.mouseX = x;
	ctx_.mouseY = y;
}

void HRL_MouseButtonCallback(int button, int pressed)
{
	if (button != HRL_MOUSE_BUTTON_LEFT)
		return;

	if (pressed == HRL_MOUSE_PRESS)
	{
		ctx_.mouseLeftDown = true;
		ctx_.mouseLeftPressed = true;
		ctx_.mouseCaptureWidget = HRL_INVALID_ID;
		ctx_.mouseCaptureGizmo = HRL_INVALID_ID;
		ctx_.mouseCaptureGizmoPart = HRL_GIZMO_PART_NONE;
	}
	else if (pressed == HRL_MOUSE_RELEASE)
	{
		ctx_.mouseLeftDown = false;
		ctx_.mouseLeftReleased = true;
	}
	else
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_MouseButtonCallback: pressed must be HRL_MOUSE_PRESS or HRL_MOUSE_RELEASE");
	}
}

HRL_id HRL_CreateWidget(HRL_id viewport, HRL_EWidgetType type)
{
	auto it = ctx_.viewports.find(viewport);
	if (it == ctx_.viewports.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateWidget: invalid viewport ID");
		return HRL_INVALID_ID;
	}

	HRL_id newId = GenerateHRL_ID();
	HRL_Widget* widget = nullptr;
	switch (type)
	{
	case HRL_WIDGET_BUTTON:      widget = new HRL_WidgetButton{}; break;
	case HRL_WIDGET_LABEL:       widget = new HRL_WidgetLabel{}; break;
	case HRL_WIDGET_IMAGE:       widget = new HRL_WidgetImage{}; break;
	case HRL_WIDGET_SLIDER:      widget = new HRL_WidgetSlider{}; break;
	case HRL_WIDGET_CHECKBOX:    widget = new HRL_WidgetCheckbox{}; break;
	case HRL_WIDGET_PROGRESSBAR: widget = new HRL_WidgetProgressBar{}; break;
	default:
		SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_CreateWidget: unsupported widget type");
		return HRL_INVALID_ID;
	}

	widget->SetId(newId);
	widget->SetViewport(viewport);
	ctx_.widgets.emplace(newId, widget);
	it->second->widgets.emplace(newId, widget);
	return newId;
}

void HRL_DeleteWidget(HRL_id widget)
{
	auto it = ctx_.widgets.find(widget);
	if (it == ctx_.widgets.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteWidget: invalid widget ID");
		return;
	}

	for (auto& [viewportId, viewport] : ctx_.viewports)
	{
		(void)viewportId;
		if (viewport) viewport->widgets.erase(widget);
	}

	delete it->second;
	ctx_.widgets.erase(it);
	if (ctx_.mouseCaptureWidget == widget)
		ctx_.mouseCaptureWidget = HRL_INVALID_ID;
}

int HRL_IsValidWidget(HRL_id widget)
{
	return FindWidgetConst(widget) ? HRL_TRUE : HRL_FALSE;
}

void HRL_SetWidgetPosition(HRL_id widget, float x, float y)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetPosition: invalid widget ID");
		return;
	}
	w->SetPosition(x, y);
}

void HRL_SetWidgetWorldPosition(HRL_id widget, float x, float y, float z)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetWorldPosition: invalid widget ID");
		return;
	}
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetWidgetWorldPosition: coordinates must be finite");
		return;
	}
	w->SetWorldPosition(x, y, z);
}

void HRL_SetWidgetWorldPositionEnabled(HRL_id widget, int enabled)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetWorldPositionEnabled: invalid widget ID");
		return;
	}
	w->SetWorldPositionEnabled(enabled != HRL_FALSE);
}

int HRL_IsWidgetWorldPositionEnabled(HRL_id widget)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_IsWidgetWorldPositionEnabled: invalid widget ID");
		return HRL_FALSE;
	}
	return w->IsWorldPositionEnabled() ? HRL_TRUE : HRL_FALSE;
}

void HRL_SetWidgetSize(HRL_id widget, float width, float height)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetSize: invalid widget ID");
		return;
	}
	w->SetScale(width, height);
}

void HRL_SetWidgetAlpha(HRL_id widget, float a)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetAlpha: invalid widget ID");
		return;
	}
	w->SetAlpha(a);
}

void HRL_SetWidgetAnchor(HRL_id widget, float ax, float ay)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetAnchor: invalid widget ID");
		return;
	}
	w->SetAnchor(ax, ay);
}

void HRL_SetWidgetVisible(HRL_id widget, int visible)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetVisible: invalid widget ID");
		return;
	}
	w->SetVisible(visible != HRL_FALSE);
}

void HRL_SetWidgetEnabled(HRL_id widget, int enabled)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetEnabled: invalid widget ID");
		return;
	}
	w->SetEnabled(enabled != HRL_FALSE);
}

void HRL_SetWidgetZIndex(HRL_id widget, int z_index)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetZIndex: invalid widget ID");
		return;
	}
	w->SetZIndex(z_index);
}

int HRL_IsWidgetHovered(HRL_id widget)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_IsWidgetHovered: invalid widget ID");
		return HRL_FALSE;
	}

	auto viewportIt = ctx_.viewports.find(w->GetViewport());
	if (viewportIt == ctx_.viewports.end() || !viewportIt->second)
		return HRL_FALSE;

	HRL_Viewport* viewport = viewportIt->second;
	w->UpdateInput(ctx_.mouseX, ctx_.mouseY,
		ctx_.mouseLeftDown, ctx_.mouseLeftPressed, ctx_.mouseLeftReleased,
		viewport->x_ * static_cast<float>(ctx_.window_width),
		viewport->y_ * static_cast<float>(ctx_.window_height),
		viewport->width_ * static_cast<float>(ctx_.window_width),
		viewport->height_ * static_cast<float>(ctx_.window_height));
	return w->IsHovered() ? HRL_TRUE : HRL_FALSE;
}

// BUTTON WIDGET
void HRL_SetButtonClickable(HRL_id widget, int clickable)
{
	if (auto* w = FindTypedWidget<HRL_WidgetButton>(widget, "HRL_SetButtonClickable"))
		w->SetClickable(clickable != HRL_FALSE);
}

void HRL_SetButtonText(HRL_id widget, const char* text)
{
	if (auto* w = FindTypedWidget<HRL_WidgetButton>(widget, "HRL_SetButtonText"))
		w->SetText(text);
}

void HRL_SetButtonTextSize(HRL_id widget, float size)
{
	if (auto* w = FindTypedWidget<HRL_WidgetButton>(widget, "HRL_SetButtonTextSize"))
		w->SetTextSize(size);
}

void HRL_SetButtonTextTintColor(HRL_id widget, HRL_EWidgetState state, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetButton>(widget, "HRL_SetButtonTextTintColor"))
	{
		const int index = WidgetStateIndex(state);
		if (index < 0)
		{
			SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_SetButtonTextTintColor: invalid widget state");
			return;
		}
		w->SetTextTintColor(state, {r, g, b, a});
	}
}

void HRL_SetButtonTextFont(HRL_id widget, HRL_id font)
{
	if (auto* w = FindTypedWidget<HRL_WidgetButton>(widget, "HRL_SetButtonTextFont"))
	{
		if (!HRL_IsValidFont(font))
		{
			SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetButtonTextFont: invalid font ID");
			return;
		}
		w->SetFont(font);
	}
}

void HRL_SetButtonTextLayout(HRL_id widget, HRL_ETextLayout layout)
{
	if (auto* w = FindTypedWidget<HRL_WidgetButton>(widget, "HRL_SetButtonTextLayout"))
	{
		if (layout < HRL_TEXT_LAYOUT_FIT || layout > HRL_TEXT_LAYOUT_CENTER)
		{
			SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_SetButtonTextLayout: invalid layout");
			return;
		}
		w->text_layout_ = layout;
	}
}

void HRL_SetButtonBackgroundTexture(HRL_id widget, HRL_EWidgetState state, HRL_id texture)
{
	if (auto* w = FindTypedWidget<HRL_WidgetButton>(widget, "HRL_SetButtonBackgroundTexture"))
	{
		const int index = WidgetStateIndex(state);
		if (index < 0)
		{
			SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_SetButtonBackgroundTexture: invalid widget state");
			return;
		}
		// Keep the historical API contract: texture 0 means the default white texture.
		if (texture == 0 || texture == HRL_INVALID_ID)
			texture = HRL_INVALID_ID;
		else if (!HRL_IsValidTexture(texture))
		{
			SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetButtonBackgroundTexture: invalid texture ID");
			return;
		}
		w->SetBackgroundTexture(state, texture);
	}
}

void HRL_SetButtonBackgroundTintColor(HRL_id widget, HRL_EWidgetState state, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetButton>(widget, "HRL_SetButtonBackgroundTintColor"))
	{
		if (WidgetStateIndex(state) < 0)
		{
			SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_SetButtonBackgroundTintColor: invalid widget state");
			return;
		}
		w->SetBackgroundTintColor(state, {r, g, b, a});
	}
}

void HRL_SetButtonPressedCallback(HRL_id widget, HRL_CButtonPressed callback, void* user_data)
{
	if (auto* w = FindTypedWidget<HRL_WidgetButton>(widget, "HRL_SetButtonPressedCallback"))
	{
		w->pressed_callback = callback;
		w->pressed_user_data = user_data;
	}
}

// LABEL WIDGET
void HRL_SetLabelText(HRL_id widget, const char* text)
{
	if (auto* w = FindTypedWidget<HRL_WidgetLabel>(widget, "HRL_SetLabelText"))
		w->SetText(text);
}

void HRL_SetLabelTextSize(HRL_id widget, float size)
{
	if (auto* w = FindTypedWidget<HRL_WidgetLabel>(widget, "HRL_SetLabelTextSize"))
		w->SetTextSize(size);
}

void HRL_SetLabelFont(HRL_id widget, HRL_id font)
{
	if (auto* w = FindTypedWidget<HRL_WidgetLabel>(widget, "HRL_SetLabelFont"))
	{
		if (!HRL_IsValidFont(font))
		{
			SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLabelFont: invalid font ID");
			return;
		}
		w->SetFont(font);
	}
}

void HRL_SetLabelTintColor(HRL_id widget, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetLabel>(widget, "HRL_SetLabelTintColor"))
		w->SetTintColor({r, g, b, a});
}

void HRL_SetLabelTextLayout(HRL_id widget, HRL_ETextLayout layout)
{
	if (auto* w = FindTypedWidget<HRL_WidgetLabel>(widget, "HRL_SetLabelTextLayout"))
	{
		if (layout < HRL_TEXT_LAYOUT_FIT || layout > HRL_TEXT_LAYOUT_CENTER)
		{
			SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_SetLabelTextLayout: invalid layout");
			return;
		}
		w->text_layout_ = layout;
	}
}

// IMAGE WIDGET
void HRL_SetImageTexture(HRL_id widget, HRL_id texture)
{
	if (auto* w = FindTypedWidget<HRL_WidgetImage>(widget, "HRL_SetImageTexture"))
	{
		if (texture != HRL_INVALID_ID && !HRL_IsValidTexture(texture))
		{
			SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetImageTexture: invalid texture ID");
			return;
		}
		w->texture_ = texture;
	}
}

void HRL_SetImageTintColor(HRL_id widget, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetImage>(widget, "HRL_SetImageTintColor"))
		w->tint_color_ = {r, g, b, a};
}

// SLIDER WIDGET
void HRL_SetSliderRange(HRL_id widget, float minimum, float maximum)
{
	if (auto* w = FindTypedWidget<HRL_WidgetSlider>(widget, "HRL_SetSliderRange"))
	{
		if (maximum < minimum)
			std::swap(minimum, maximum);
		w->minimum_ = minimum;
		w->maximum_ = maximum;
		w->SetValue(w->value_);
	}
}

void HRL_SetSliderValue(HRL_id widget, float value)
{
	if (auto* w = FindTypedWidget<HRL_WidgetSlider>(widget, "HRL_SetSliderValue"))
		w->SetValue(value);
}

float HRL_GetSliderValue(HRL_id widget)
{
	if (auto* w = FindTypedWidget<HRL_WidgetSlider>(widget, "HRL_GetSliderValue"))
		return w->value_;
	return 0.0f;
}

void HRL_SetSliderOrientation(HRL_id widget, HRL_ESliderOrientation orientation)
{
	if (auto* w = FindTypedWidget<HRL_WidgetSlider>(widget, "HRL_SetSliderOrientation"))
	{
		if (orientation != HRL_SLIDER_HORIZONTAL && orientation != HRL_SLIDER_VERTICAL)
		{
			SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_SetSliderOrientation: invalid orientation");
			return;
		}
		w->orientation_ = orientation;
	}
}

void HRL_SetSliderClickable(HRL_id widget, int clickable)
{
	if (auto* w = FindTypedWidget<HRL_WidgetSlider>(widget, "HRL_SetSliderClickable"))
		w->clickable_ = clickable != HRL_FALSE;
}

void HRL_SetSliderBackgroundColor(HRL_id widget, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetSlider>(widget, "HRL_SetSliderBackgroundColor"))
		w->background_color_ = {r, g, b, a};
}

void HRL_SetSliderFillColor(HRL_id widget, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetSlider>(widget, "HRL_SetSliderFillColor"))
		w->fill_color_ = {r, g, b, a};
}

void HRL_SetSliderHandleColor(HRL_id widget, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetSlider>(widget, "HRL_SetSliderHandleColor"))
		w->handle_color_ = {r, g, b, a};
}

void HRL_SetSliderChangedCallback(HRL_id widget, HRL_CSliderChanged callback, void* user_data)
{
	if (auto* w = FindTypedWidget<HRL_WidgetSlider>(widget, "HRL_SetSliderChangedCallback"))
	{
		w->changed_callback = callback;
		w->changed_user_data = user_data;
	}
}

// CHECKBOX WIDGET
void HRL_SetCheckboxChecked(HRL_id widget, int checked)
{
	if (auto* w = FindTypedWidget<HRL_WidgetCheckbox>(widget, "HRL_SetCheckboxChecked"))
		w->SetChecked(checked != HRL_FALSE);
}

int HRL_IsCheckboxChecked(HRL_id widget)
{
	if (auto* w = FindTypedWidget<HRL_WidgetCheckbox>(widget, "HRL_IsCheckboxChecked"))
		return w->checked_ ? HRL_TRUE : HRL_FALSE;
	return HRL_FALSE;
}

void HRL_SetCheckboxClickable(HRL_id widget, int clickable)
{
	if (auto* w = FindTypedWidget<HRL_WidgetCheckbox>(widget, "HRL_SetCheckboxClickable"))
		w->clickable_ = clickable != HRL_FALSE;
}

void HRL_SetCheckboxBackgroundColor(HRL_id widget, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetCheckbox>(widget, "HRL_SetCheckboxBackgroundColor"))
		w->background_color_ = {r, g, b, a};
}

void HRL_SetCheckboxCheckedColor(HRL_id widget, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetCheckbox>(widget, "HRL_SetCheckboxCheckedColor"))
		w->checked_color_ = {r, g, b, a};
}

void HRL_SetCheckboxChangedCallback(HRL_id widget, HRL_CCheckboxChanged callback, void* user_data)
{
	if (auto* w = FindTypedWidget<HRL_WidgetCheckbox>(widget, "HRL_SetCheckboxChangedCallback"))
	{
		w->changed_callback = callback;
		w->changed_user_data = user_data;
	}
}

// PROGRESS BAR WIDGET
void HRL_SetProgressBarValue(HRL_id widget, float value)
{
	if (auto* w = FindTypedWidget<HRL_WidgetProgressBar>(widget, "HRL_SetProgressBarValue"))
		w->SetValue(value);
}

float HRL_GetProgressBarValue(HRL_id widget)
{
	if (auto* w = FindTypedWidget<HRL_WidgetProgressBar>(widget, "HRL_GetProgressBarValue"))
		return w->value_;
	return 0.0f;
}

void HRL_SetProgressBarBackgroundColor(HRL_id widget, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetProgressBar>(widget, "HRL_SetProgressBarBackgroundColor"))
		w->background_color_ = {r, g, b, a};
}

void HRL_SetProgressBarFillColor(HRL_id widget, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetProgressBar>(widget, "HRL_SetProgressBarFillColor"))
		w->fill_color_ = {r, g, b, a};
}
