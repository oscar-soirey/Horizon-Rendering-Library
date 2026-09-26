#include "utils_functions.h"

#include "../hrl.h"

#include <stb/stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstdint>


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

namespace {

static std::vector<uint32_t> DecodeUTF8(const char* text)
{
    std::vector<uint32_t> codepoints;
    if (!text)
        return codepoints;

    const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
    while (*p)
    {
        uint32_t cp = 0xFFFDu;
        size_t length = 1;
        const unsigned char c0 = *p;

        if (c0 < 0x80u)
        {
            cp = c0;
            length = 1;
        }
        else if ((c0 & 0xE0u) == 0xC0u && p[1] != 0)
        {
            const unsigned char c1 = p[1];
            if ((c1 & 0xC0u) == 0x80u)
            {
                cp = (uint32_t(c0 & 0x1Fu) << 6) | uint32_t(c1 & 0x3Fu);
                if (cp >= 0x80u) length = 2; else cp = 0xFFFDu;
            }
        }
        else if ((c0 & 0xF0u) == 0xE0u && p[1] != 0 && p[2] != 0)
        {
            const unsigned char c1 = p[1];
            const unsigned char c2 = p[2];
            if ((c1 & 0xC0u) == 0x80u && (c2 & 0xC0u) == 0x80u)
            {
                cp = (uint32_t(c0 & 0x0Fu) << 12) |
                     (uint32_t(c1 & 0x3Fu) << 6) |
                     uint32_t(c2 & 0x3Fu);
                if (cp >= 0x800u && !(cp >= 0xD800u && cp <= 0xDFFFu)) length = 3;
                else cp = 0xFFFDu;
            }
        }
        else if ((c0 & 0xF8u) == 0xF0u && p[1] != 0 && p[2] != 0 && p[3] != 0)
        {
            const unsigned char c1 = p[1];
            const unsigned char c2 = p[2];
            const unsigned char c3 = p[3];
            if ((c1 & 0xC0u) == 0x80u && (c2 & 0xC0u) == 0x80u && (c3 & 0xC0u) == 0x80u)
            {
                cp = (uint32_t(c0 & 0x07u) << 18) |
                     (uint32_t(c1 & 0x3Fu) << 12) |
                     (uint32_t(c2 & 0x3Fu) << 6) |
                     uint32_t(c3 & 0x3Fu);
                if (cp >= 0x10000u && cp <= 0x10FFFFu) length = 4;
                else cp = 0xFFFDu;
            }
        }

        codepoints.push_back(cp);
        p += length;
    }
    return codepoints;
}

static void DistanceTransform8(const std::vector<unsigned char>& mask, int width, int height,
                               bool targetIsInside, std::vector<float>& out)
{
    const float inf = 1.0e20f;
    const float diag = 1.41421356237f;
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    out.assign(count, inf);

    for (size_t i = 0; i < count; ++i)
    {
        const bool inside = mask[i] != 0;
        if (inside == targetIsInside)
            out[i] = 0.0f;
    }

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const size_t i = static_cast<size_t>(y) * width + x;
            float d = out[i];
            if (x > 0) d = std::min(d, out[i - 1] + 1.0f);
            if (y > 0) d = std::min(d, out[i - width] + 1.0f);
            if (x > 0 && y > 0) d = std::min(d, out[i - width - 1] + diag);
            if (x + 1 < width && y > 0) d = std::min(d, out[i - width + 1] + diag);
            out[i] = d;
        }
    }

    for (int y = height - 1; y >= 0; --y)
    {
        for (int x = width - 1; x >= 0; --x)
        {
            const size_t i = static_cast<size_t>(y) * width + x;
            float d = out[i];
            if (x + 1 < width) d = std::min(d, out[i + 1] + 1.0f);
            if (y + 1 < height) d = std::min(d, out[i + width] + 1.0f);
            if (x + 1 < width && y + 1 < height) d = std::min(d, out[i + width + 1] + diag);
            if (x > 0 && y + 1 < height) d = std::min(d, out[i + width - 1] + diag);
            out[i] = d;
        }
    }
}

struct SDFGlyphPlacement
{
    uint32_t codepoint = 0;
    float x = 0.0f;
    float baseline = 0.0f;
};

} // namespace

BitmapResult GenerateSDFBitmap(
    const char* text, stbtt_fontinfo* font,
    const std::vector<unsigned char>& ttf_buffer,
    float font_size, float wrap_width
)
{
    BitmapResult out{};
    if (!text || !font || ttf_buffer.empty() || font_size <= 0.0f)
        return out;

    const std::vector<uint32_t> codepoints = DecodeUTF8(text);
    if (codepoints.empty())
        return out;

    // Render at a reasonably high resolution. The resulting distance field can
    // then be minified or magnified by the UI shader without becoming a bitmap.
    const float generation_size = std::max(font_size, 48.0f);
    const float scale = stbtt_ScaleForPixelHeight(font, generation_size);
    int ascent = 0, descent = 0, line_gap = 0;
    stbtt_GetFontVMetrics(font, &ascent, &descent, &line_gap);

    const int padding = std::max(8, static_cast<int>(std::ceil(generation_size * 0.20f)));
    const float line_height = std::max(1.0f,
        (static_cast<float>(ascent - descent + line_gap) * scale));

    std::vector<SDFGlyphPlacement> placements;
    placements.reserve(codepoints.size());

    float pen_x = 0.0f;
    float baseline_y = 0.0f;
    float max_line_width = 0.0f;
    int line_count = 1;
    uint32_t previous = 0;

    for (uint32_t cp : codepoints)
    {
        if (cp == '\n')
        {
            max_line_width = std::max(max_line_width, pen_x);
            pen_x = 0.0f;
            baseline_y += line_height;
            ++line_count;
            previous = 0;
            continue;
        }
        if (cp == '\r')
            continue;

        if (cp == '\t')
            cp = ' ';

        const int glyphIndex = stbtt_FindGlyphIndex(font, static_cast<int>(cp));
        if (glyphIndex == 0 && cp != 0)
        {
            // Replacement glyph makes the behavior usable for ordinary UTF-8
            // text even when a font does not contain a requested codepoint.
            cp = 0xFFFDu;
        }

        if (previous != 0)
            pen_x += stbtt_GetCodepointKernAdvance(font,
                static_cast<int>(previous), static_cast<int>(cp)) * scale;

        int advance_width = 0, left_side_bearing = 0;
        stbtt_GetCodepointHMetrics(font, static_cast<int>(cp), &advance_width, &left_side_bearing);
        const float advance = std::max(0.0f, advance_width * scale);

        if (wrap_width > 0.0f && pen_x > 0.0f && pen_x + advance > wrap_width)
        {
            max_line_width = std::max(max_line_width, pen_x);
            pen_x = 0.0f;
            baseline_y += line_height;
            ++line_count;
            previous = 0;
        }

        placements.push_back({cp, pen_x, baseline_y});
        pen_x += advance;
        previous = cp;
    }
    max_line_width = std::max(max_line_width, pen_x);

    const int width = std::max(1, static_cast<int>(std::ceil(max_line_width)) + padding * 2);
    const int height = std::max(1, static_cast<int>(std::ceil(line_count * line_height)) + padding * 2);

    std::vector<unsigned char> coverage(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    const float firstBaseline = padding + ascent * scale;

    for (const SDFGlyphPlacement& placement : placements)
    {
        int glyph_w = 0, glyph_h = 0, xoff = 0, yoff = 0;
        unsigned char* glyph = stbtt_GetCodepointBitmap(
            font, scale, scale, static_cast<int>(placement.codepoint),
            &glyph_w, &glyph_h, &xoff, &yoff);
        if (!glyph)
            continue;

        const int dst_x = static_cast<int>(std::floor(padding + placement.x + xoff));
        const int dst_y = static_cast<int>(std::floor(firstBaseline + placement.baseline + yoff));
        for (int gy = 0; gy < glyph_h; ++gy)
        {
            const int dy = dst_y + gy;
            if (dy < 0 || dy >= height)
                continue;
            for (int gx = 0; gx < glyph_w; ++gx)
            {
                const int dx = dst_x + gx;
                if (dx < 0 || dx >= width)
                    continue;
                const unsigned char value = glyph[gy * glyph_w + gx];
                unsigned char& dst = coverage[static_cast<size_t>(dy) * width + dx];
                if (value > dst)
                    dst = value;
            }
        }
        stbtt_FreeBitmap(glyph, nullptr);
    }

    std::vector<float> distanceToOutside;
    std::vector<float> distanceToInside;
    DistanceTransform8(coverage, width, height, false, distanceToOutside);
    DistanceTransform8(coverage, width, height, true, distanceToInside);

    // Store the SDF in all three color channels and keep alpha at one. The UI
    // shader samples the red channel and reconstructs coverage with derivatives.
    const float distanceRange = static_cast<float>(padding);
    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const size_t i = static_cast<size_t>(y) * width + x;
            const bool inside = coverage[i] >= 128;
            float signedDistance = inside ? distanceToOutside[i] : -distanceToInside[i];
            signedDistance = std::clamp(signedDistance, -distanceRange, distanceRange);

            // Move the SDF edge slightly according to the original coverage so
            // antialiased source pixels do not turn into a hard binary contour.
            if (coverage[i] != 0 && coverage[i] != 255)
                signedDistance += (static_cast<float>(coverage[i]) / 255.0f - 0.5f) * 0.75f;

            const float normalized = std::clamp(0.5f + signedDistance / (2.0f * distanceRange), 0.0f, 1.0f);
            const unsigned char sdf = static_cast<unsigned char>(std::lround(normalized * 255.0f));
            const size_t o = i * 4u;
            out.pixels[o + 0] = sdf;
            out.pixels[o + 1] = sdf;
            out.pixels[o + 2] = sdf;
            out.pixels[o + 3] = 255u;
        }
    }

    return out;
}
