#ifndef GLES3_TEXTURE
#define GLES3_TEXTURE

#include "../../hrl.h"
#include "gles3_gl.h"

struct BitmapResult;

// 2D RGBA8 texture. Unlike the desktop backend, no CPU copy of the pixels is
// kept after the upload: memory is the scarce resource on mobile.
class GLES3_Texture final {
public:
  GLES3_Texture()=default;
  ~GLES3_Texture();

  GLES3_Texture(const GLES3_Texture&) = delete;
  GLES3_Texture& operator=(const GLES3_Texture&) = delete;

  //retourne 0 pour pas d'erreur et -1 pour erreur
  // collapseUniform : an image made of a single color is stored as 1x1.
  int GLES3_Create(const char* _imageContent, const size_t _imageSize, bool collapseUniform = false);
  int GLES3_CreateFromBitmap(BitmapResult* bmp);

  HRL_uint GetWidth() const { return static_cast<HRL_uint>(width_); }
  HRL_uint GetHeight() const { return static_cast<HRL_uint>(height_); }
  GLuint GetGL_ID() const { return glID_; }

  // First texel (bottom-left once uploaded), 0..255. Used for the fallbacks.
  const unsigned char* GetFirstTexel() const { return first_texel_; }

  void SetMinFilter(HRL_uint filter);
  void SetMaxFilter(HRL_uint filter);

private:
  int Upload(const unsigned char* rgbaTopDown, int width, int height, bool collapseUniform);

  GLuint glID_ = 0;
  GLint width_ = 0, height_ = 0;
  unsigned char first_texel_[4] = {0, 0, 0, 0};
  bool mipmaps_generated_ = false;
};

#endif
