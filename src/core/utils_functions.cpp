#include "utils_functions.h"

#include "../hrl.h"

#include <stb/stb_truetype.h>


extern HRL_Context* GetPrivateContext();

void SetErrorCode(HRL_EError e, HRL_ESeverity severity, const std::string& detail)
{
	GetPrivateContext()->last_error.code = e;
	GetPrivateContext()->last_error.severity = severity;
	GetPrivateContext()->last_error.detail = detail;
	if (GetPrivateContext()->error_callback)
	{
		GetPrivateContext()->error_callback(e, severity, detail.c_str());
	}
}


//Generer un id pour les objets HRL
static HRL_id currentID = 1;
HRL_id GenerateHRL_ID()
{
	return currentID++;
}


//Window
unsigned int GetWindowWidth()
{
	return GetPrivateContext()->window_width;
}
extern unsigned int window_height_;
unsigned int GetWindowHeight()
{
	return GetPrivateContext()->window_height;
}


glm::vec3 GetForwardVector(glm::vec3 _rot)
{
  glm::vec3 forward;
  forward.x = cos(glm::radians(_rot.x)) * cos(glm::radians(_rot.y));
  forward.y = sin(glm::radians(_rot.x));
  forward.z = cos(glm::radians(_rot.x)) * sin(glm::radians(_rot.y));
  return glm::normalize(forward);
}
glm::vec3 GetRightVector(glm::vec3 _rot)
{
  return glm::normalize(glm::cross(
    GetForwardVector(_rot),
    glm::vec3(0.0f, 1.0f, 0.0f)
  ));
}
glm::vec3 GetUpVector(glm::vec3 _rot)
{
  return glm::normalize(glm::cross(
    GetRightVector(_rot),
    GetForwardVector(_rot)
  ));
}


BitmapResult GenerateBitmap(
	const char* text, stbtt_fontinfo* font,
  const std::vector<unsigned char>& ttf_buffer,
  float font_size, float wrap_width,
  float r, float g, float b,
  float bg_r, float bg_g, float bg_b, float bg_a
)
{
    // 1. Bake la font dans un atlas
		//calcul de la taille approximative nécessaire : ~(font_size * 10)^2
		int atlas_size = std::max(512, (int)(font_size * 10));
		// Arrondir à la puissance de 2 supérieure
		int ATLAS = 1;
		while (ATLAS < atlas_size) ATLAS <<= 1;

    stbtt_bakedchar glyphs[96];
    std::vector<unsigned char> atlas(ATLAS * ATLAS);
    stbtt_BakeFontBitmap(ttf_buffer.data(), 0, font_size, atlas.data(), ATLAS, ATLAS, 32, 96, glyphs);

    // 2. Mesure la taille du bitmap de sortie
    float cx = 0, max_x = 0;
    int lines = 1;
    for (const char* c = text; *c; c++) {
        if (*c == '\n' || (wrap_width > 0 && cx >= wrap_width)) {
            max_x = std::max(max_x, cx);
            cx = 0; lines++;
            continue;
        }
        if (*c < 32 || *c >= 128) continue;
        cx += glyphs[(int)(*c - 32)].xadvance;
    }
    max_x = std::max(max_x, cx);

    int tex_w = (wrap_width > 0) ? (int)wrap_width : (int)max_x;
    int tex_h = lines * (int)font_size;

    // 3. Remplir le fond
    BitmapResult out;
    out.width = tex_w; out.height = tex_h;
    out.pixels.resize(tex_w * tex_h * 4);
    for (int i = 0; i < tex_w * tex_h; i++) {
        out.pixels[i*4+0] = (unsigned char)(bg_r * 255);
        out.pixels[i*4+1] = (unsigned char)(bg_g * 255);
        out.pixels[i*4+2] = (unsigned char)(bg_b * 255);
        out.pixels[i*4+3] = (unsigned char)(bg_a * 255);
    }

    // 4. Coller les glyphes
    float pen_x = 0, pen_y = font_size;
    for (const char* c = text; *c; c++) {
        if (*c == '\n' || (wrap_width > 0 && pen_x >= wrap_width)) {
            pen_x = 0; pen_y += font_size; continue;
        }
        if (*c < 32 || *c >= 128) continue;

        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(glyphs, ATLAS, ATLAS, *c - 32, &pen_x, &pen_y, &q, 1);

        int x0 = (int)q.x0, y0 = (int)q.y0;
        int x1 = (int)q.x1, y1 = (int)q.y1;

        for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            if (x < 0 || x >= tex_w || y < 0 || y >= tex_h) continue;
            float u = q.s0 + (q.s1-q.s0) * (float)(x-x0)/(float)(x1-x0);
            float v = q.t0 + (q.t1-q.t0) * (float)(y-y0)/(float)(y1-y0);
            float a = (float)atlas[(int)(v*ATLAS)*ATLAS + (int)(u*ATLAS)] / 255.0f;
            int idx = (y * tex_w + x) * 4;
            out.pixels[idx+0] = (unsigned char)((r   * a + bg_r*(1-a)) * 255);
            out.pixels[idx+1] = (unsigned char)((g   * a + bg_g*(1-a)) * 255);
            out.pixels[idx+2] = (unsigned char)((b   * a + bg_b*(1-a)) * 255);
            out.pixels[idx+3] = (unsigned char)(std::min(1.0f, bg_a + a) * 255);
        }
    }

    return out;
}