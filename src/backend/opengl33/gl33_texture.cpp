#include "gl33_texture.h"

#include "../../core/utils_functions.h"

/** Loader OpenGL */
#include <glad/glad.h>

/** Librarie permettant de décoder les images png, jpg, jpeg, ... */
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#include <string>
#include <unordered_map>

int GL33_Texture::GL33_Create(const char* _imageContent, const size_t _imageSize)
{
  //on crée la texture OpenGL
  glGenTextures(1, &glID_);
  glBindTexture(GL_TEXTURE_2D, glID_);

  //configuration des parametres de la texture
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  //filtrage trilinéaire
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  stbi_set_flip_vertically_on_load(true);

  // Convertir std::vector<char>::data() en unsigned char* et sa taille en int
  unsigned char* data = stbi_load_from_memory(
    (stbi_uc const*)_imageContent,        //Pointeur vers les données
    (int)_imageSize,            //Taille totale du tampon
    &width_,
    &height_,
    &nr_channels_,
    STBI_rgb_alpha
  );

  if (data)
  {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    //on libere la mémoire allouée par stb
    stbi_image_free(data);

    return 0;
  }
  else
  {
    SetErrorCode("Texture failed to load");

    //on libere la memoire allouée par opengl, stb et la texture elle meme
    glDeleteTextures(1, &glID_);

    //on libere la mémoire allouée par stb (normalement c'est pas nécéssaire mais on le fait quand meme)
    stbi_image_free(data);

    return -1;
  }
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

void GL33_Texture::Resize(HRL_uint width, HRL_uint height)
{
  //fonction pas encore implémentée
}