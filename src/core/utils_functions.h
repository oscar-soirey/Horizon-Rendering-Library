/**
 * Fonctions utiles pour la communication entre l'API (l'utilisateur final) et le backend
 */

#ifndef HRL_UTILS_FUNCTIONS
#define HRL_UTILS_FUNCTIONS

#include "../hrl.h"
#include "object_types.h"

#include <string>
#include <vector>

#include <glm/glm.hpp>

void SetErrorCode(const std::string& e);

HRL_id GenerateHRL_ID();

unsigned int GetWindowWidth();
unsigned int GetWindowHeight();

glm::vec3 GetForwardVector(glm::vec3 _rotation);
glm::vec3 GetRightVector(glm::vec3 _rotation);
glm::vec3 GetUpVector(glm::vec3 _rotation);

HRL_uint GetTextureMinFilter();
HRL_uint GetTextureMagFilter();


//generate bitmap from text
class stbtt_fontinfo;
BitmapResult GenerateBitmap(
		const char* text, stbtt_fontinfo* font,
		const std::vector<unsigned char>& ttf_buffer,
		float font_size, float wrap_width,
		float r, float g, float b,
		float bg_r, float bg_g, float bg_b, float bg_a
);

#endif