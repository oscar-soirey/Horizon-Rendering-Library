#include "gl33_texture.h"

#include "../../core/utils_functions.h"

/** Loader OpenGL */
#include <glad/glad.h>

/** Librarie permettant de décoder les images png, jpg, jpeg, ... */
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#include <string>
#include <cstring>
#include <limits>

int GL33_Texture::GL33_Create(const char* _imageContent, const size_t _imageSize)
{
  if (!_imageContent || _imageSize == 0 || _imageSize > static_cast<size_t>(std::numeric_limits<int>::max()))
  {
    SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "GL33_Texture::GL33_Create: invalid image buffer");
    return -1;
  }

  glGenTextures(1, &glID_);
  glBindTexture(GL_TEXTURE_2D, glID_);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  int decodedChannels = 0;
  unsigned char* data = stbi_load_from_memory(
    reinterpret_cast<const stbi_uc*>(_imageContent),
    static_cast<int>(_imageSize),
    &width_,
    &height_,
    &decodedChannels,
    STBI_rgb_alpha);
  nr_channels_ = 4;

  if (!data || width_ <= 0 || height_ <= 0)
  {
    SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "Texture failed to load");
    stbi_image_free(data);
    glDeleteTextures(1, &glID_);
    glID_ = 0;
    return -1;
  }

  cpu_rgba_.resize(size_t(width_) * size_t(height_) * 4u);
  const size_t rowBytes = size_t(width_) * 4u;
  for (int y = 0; y < height_; ++y)
  {
    // Preserve the previous HRL/stb vertically-flipped convention without
    // touching stb_image's process-global state.
    const unsigned char* src = data + size_t(height_ - 1 - y) * rowBytes;
    std::memcpy(cpu_rgba_.data() + size_t(y) * rowBytes, src, rowBytes);
  }

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, cpu_rgba_.data());
  mipmaps_generated_ = false;

  stbi_image_free(data);
  glBindTexture(GL_TEXTURE_2D, 0);
  return 0;
}

int GL33_Texture::GL33_CreateFromBitmap(BitmapResult* bmp)
{
  if (!bmp || bmp->width <= 0 || bmp->height <= 0 ||
      bmp->pixels.size() != size_t(bmp->width) * size_t(bmp->height) * 4u)
  {
    SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "GL33_Texture::GL33_CreateFromBitmap: invalid bitmap");
    return -1;
  }

  width_  = bmp->width;
  height_ = bmp->height;
  nr_channels_ = 4;

  glGenTextures(1, &glID_);
  glBindTexture(GL_TEXTURE_2D, glID_);

  cpu_rgba_.resize(bmp->pixels.size());
  const size_t rowBytes = size_t(bmp->width) * 4u;
  for (int y = 0; y < bmp->height; ++y)
  {
    std::memcpy(
      cpu_rgba_.data() + size_t(y) * rowBytes,
      bmp->pixels.data() + size_t(bmp->height - 1 - y) * rowBytes,
      rowBytes);
  }

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
      bmp->width, bmp->height,
      0, GL_RGBA, GL_UNSIGNED_BYTE,
      cpu_rgba_.data());
  mipmaps_generated_ = false;

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  glBindTexture(GL_TEXTURE_2D, 0);
  return 0;
}

GL33_Texture::~GL33_Texture()
{
  glDeleteTextures(1, &glID_);
}

GLuint GL33_Texture::GetGL_ID() const
{
  return glID_;
}

HRL_uint GL33_Texture::GetWidth() const
{
  return width_;
}

HRL_uint GL33_Texture::GetHeight() const
{
  return height_;
}

void GL33_Texture::SetMinFilter(HRL_uint filter)
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

    //not avalaible with opengl 3.3
    case HRL_FILTER_ANISOTROPIC: { param = GL_LINEAR; break; }

    case HRL_FILTER_SUPERSAMPLING: { param = GL_LINEAR; break; }
    default: { param = GL_LINEAR; break; }
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, param);
}

void GL33_Texture::SetMaxFilter(HRL_uint filter)
{
  glBindTexture(GL_TEXTURE_2D, glID_);
  GLint param;
  switch (filter)
  {
    case HRL_FILTER_NEAREST: { param = GL_NEAREST; break; }
    case HRL_FILTER_LINEAR: { param = GL_LINEAR; break; }
    case HRL_FILTER_BILINEAR: { param = GL_LINEAR; break; }
    case HRL_FILTER_TRILINEAR: { param = GL_LINEAR; break; }

    //not avalaible with opengl 3.3
    case HRL_FILTER_ANISOTROPIC: { param = GL_LINEAR; break; }

    case HRL_FILTER_SUPERSAMPLING: { param = GL_LINEAR; break; }
    default: { param = GL_LINEAR; break; }
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, param);
}