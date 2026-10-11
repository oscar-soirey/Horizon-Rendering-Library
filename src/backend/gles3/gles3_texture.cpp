#include "gles3_texture.h"

#include "../../core/utils_functions.h"

/** Librarie permettant de décoder les images png, jpg, jpeg, ... */
// The stb_image implementation lives in the OpenGL 3.3 backend. When that
// backend is not compiled (mobile builds), it is instantiated here instead.
#ifdef HRL_DISABLE_OPENGL33
#define STB_IMAGE_IMPLEMENTATION
#endif
#include <stb/stb_image.h>

#include <string>
#include <cstring>
#include <limits>
#include <vector>

int GLES3_Texture::Upload(const unsigned char* rgbaTopDown, int width, int height, bool collapseUniform)
{
  const size_t rowBytes = size_t(width) * 4u;

  bool uniform = collapseUniform;
  if (uniform)
  {
    const size_t texelCount = size_t(width) * size_t(height);
    for (size_t i = 1; i < texelCount; ++i)
    {
      if (std::memcmp(rgbaTopDown, rgbaTopDown + i * 4u, 4u) != 0)
      {
        uniform = false;
        break;
      }
    }
  }

  glGenTextures(1, &glID_);
  if (glID_ == 0)
  {
    SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "GLES3_Texture: glGenTextures failed");
    return -1;
  }
  glBindTexture(GL_TEXTURE_2D, glID_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  if (uniform)
  {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgbaTopDown);
    std::memcpy(first_texel_, rgbaTopDown, 4u);
  }
  else
  {
    // HRL convention: the image is stored bottom-up (v = 0 is the bottom row).
    std::vector<unsigned char> flipped(rowBytes * size_t(height));
    for (int y = 0; y < height; ++y)
      std::memcpy(flipped.data() + size_t(y) * rowBytes, rgbaTopDown + size_t(height - 1 - y) * rowBytes, rowBytes);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data());
    std::memcpy(first_texel_, flipped.data(), 4u);
  }

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glBindTexture(GL_TEXTURE_2D, 0);

  // The logical size stays the one of the source image (HRL_GetTextureSize).
  width_ = width;
  height_ = height;
  mipmaps_generated_ = false;
  return 0;
}

int GLES3_Texture::GLES3_Create(const char* _imageContent, const size_t _imageSize, bool collapseUniform)
{
  if (!_imageContent || _imageSize == 0 || _imageSize > static_cast<size_t>(std::numeric_limits<int>::max()))
  {
    SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "GLES3_Texture::GLES3_Create: invalid image buffer");
    return -1;
  }

  int width = 0, height = 0, decodedChannels = 0;
  unsigned char* data = stbi_load_from_memory(
    reinterpret_cast<const stbi_uc*>(_imageContent),
    static_cast<int>(_imageSize),
    &width, &height, &decodedChannels, STBI_rgb_alpha);

  if (!data || width <= 0 || height <= 0)
  {
    SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "Texture failed to load");
    stbi_image_free(data);
    return -1;
  }

  const int result = Upload(data, width, height, collapseUniform);
  stbi_image_free(data);
  return result;
}

int GLES3_Texture::GLES3_CreateFromBitmap(BitmapResult* bmp)
{
  if (!bmp || bmp->width <= 0 || bmp->height <= 0 ||
      bmp->pixels.size() != size_t(bmp->width) * size_t(bmp->height) * 4u)
  {
    SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "GLES3_Texture::GLES3_CreateFromBitmap: invalid bitmap");
    return -1;
  }
  return Upload(bmp->pixels.data(), bmp->width, bmp->height, false);
}

GLES3_Texture::~GLES3_Texture()
{
  if (glID_ != 0)
    glDeleteTextures(1, &glID_);
}

void GLES3_Texture::SetMinFilter(HRL_uint filter)
{
  glBindTexture(GL_TEXTURE_2D, glID_);
  GLint param;
  switch (filter)
  {
    case HRL_FILTER_NEAREST: { param = GL_NEAREST; break; }
    case HRL_FILTER_LINEAR: { param = GL_LINEAR; break; }
    case HRL_FILTER_BILINEAR:
    {
      if (!mipmaps_generated_) { glGenerateMipmap(GL_TEXTURE_2D); mipmaps_generated_ = true; }
      param = GL_LINEAR_MIPMAP_NEAREST;
      break;
    }
    case HRL_FILTER_TRILINEAR:
    {
      if (!mipmaps_generated_) { glGenerateMipmap(GL_TEXTURE_2D); mipmaps_generated_ = true; }
      param = GL_LINEAR_MIPMAP_LINEAR;
      break;
    }
    default: { param = GL_LINEAR; break; }
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, param);
}

void GLES3_Texture::SetMaxFilter(HRL_uint filter)
{
  glBindTexture(GL_TEXTURE_2D, glID_);
  const GLint param = (filter == HRL_FILTER_NEAREST) ? GL_NEAREST : GL_LINEAR;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, param);
}
