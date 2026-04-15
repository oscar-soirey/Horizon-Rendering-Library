#include "widgets.h"

#include "utils_functions.h"

//WIDGET BASE
void HRL_Widget::SetPosition(float x, float y)
{
	position_ = {x, y};
}
void HRL_Widget::SetScale(float x, float y)
{
	scale_ = {x, y};
}



HRL_WidgetButton::HRL_WidgetButton()
{

}

HRL_WidgetButton::~HRL_WidgetButton()
{

}

void HRL_WidgetButton::GetDrawInfos(std::vector<WidgetDrawInfos>& infos)
{

	WidgetDrawInfos background_geometry{position_.x, position_.y, scale_.x, scale_.y,
		background_texture_,
		background_tint_color_.x, background_tint_color_.y, background_tint_color_.z, background_tint_color_.w
	};
	infos.push_back(background_geometry);

	WidgetDrawInfos text_geometry{position_.x, position_.y, scale_.x, scale_.y,
	text_texture_,
	text_tint_color_.x, text_tint_color_.y, text_tint_color_.z, text_tint_color_.w
	};
	infos.push_back(text_geometry);
}


void HRL_WidgetButton::Logic()
{
	//printf("button logic\n");
}

void HRL_WidgetButton::GenerateTextTexture()
{
	//Delete current text texture if valid
	if (HRL_IsValidTexture(text_texture_))
	{
		HRL_DeleteTexture(text_texture_);
	}

	if (text_text_.empty())
	{
		text_texture_ = HRL_INVALID_ID;
		return;
	}

	text_texture_ = HRL_CreateTextureFromText(text_text_.c_str(), font_, text_size_*10.f, 0.f,
		text_tint_color_.x, text_tint_color_.y, text_tint_color_.z,
		0.f, 0.f, 0.f, 0.f
	);
	int w, h;
	HRL_GetTextureSize(text_texture_, &w, &h);
	printf("texture size : %d %d\n", w, h);
}
