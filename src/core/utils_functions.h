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

void SetErrorCode(HRL_EError e, HRL_ESeverity severity, const std::string& detail);

// Internal renderer access; never exposed through the public HRL API.
HRL_Context* GetPrivateContext();

HRL_id GenerateHRL_ID();

unsigned int GetWindowWidth();
unsigned int GetWindowHeight();

glm::vec3 GetForwardVector(glm::vec3 _rotation);
glm::vec3 GetRightVector(glm::vec3 _rotation);
glm::vec3 GetUpVector(glm::vec3 _rotation);


//generate bitmap from text
class stbtt_fontinfo;
BitmapResult GenerateBitmap(
		const char* text, stbtt_fontinfo* font,
		const std::vector<unsigned char>& ttf_buffer,
		float font_size, float wrap_width,
		float r, float g, float b,
		float bg_r, float bg_g, float bg_b, float bg_a
);

// SDF text textures are generated at max(font_size, 48) px, with a padding
// around the text. Widgets use the same numbers to draw a text at its real size.
inline float SDFTextGenerationSize(float font_size)
{
	return font_size > 48.0f ? font_size : 48.0f;
}
inline int SDFTextPadding(float generation_size)
{
	const int p = static_cast<int>(generation_size * 0.20f + 0.999f);
	return p > 8 ? p : 8;
}

// Internal UI text path: generates a single-channel signed distance field stored in RGBA.
BitmapResult GenerateSDFBitmap(
		const char* text, stbtt_fontinfo* font,
		const std::vector<unsigned char>& ttf_buffer,
		float font_size, float wrap_width
);

// Internal helper used by widgets so the public HRL text API remains unchanged.
HRL_id HRL_InternalCreateSDFTextTexture(const char* text, HRL_id font_id, float font_size);

#endif