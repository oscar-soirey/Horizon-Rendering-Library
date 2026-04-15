#pragma once

#include "hrl.h"
#include <glm/glm.hpp>

#include <vector>

class HRL_Widget {
public:
	HRL_Widget()=default;
	virtual ~HRL_Widget()=default;

	virtual void Logic()=0;

	void SetPosition(float x, float y);
	void SetScale(float x, float y);

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
	};

	virtual void GetDrawInfos(std::vector<WidgetDrawInfos>& infos)=0;

protected:
	glm::vec2 position_;
	glm::vec2 scale_;
};

class HRL_WidgetButton : public HRL_Widget {
public:
	HRL_WidgetButton();
	~HRL_WidgetButton() override;

	void Logic() override;

	void GetDrawInfos(std::vector<WidgetDrawInfos>& infos) override;

	void GenerateTextTexture();

	HRL_CButtonPressed pressed_callback;

public:
	HRL_id background_texture_;
	glm::vec4 background_tint_color_;

	//text
	HRL_id text_texture_;
	glm::vec4 text_tint_color_{1.f};
	float text_size_=15;
	HRL_id font_;
	std::string text_text_;

private:
	bool pressed_last_frame_=false;
};