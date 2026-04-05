#ifndef GL33_TEXTURE
#define GL33_TEXTURE

#include "../../hrl.h"
#include <glad/glad.h>

//a l'avenir, donner plus de possibilités aux textures, changer l'encodage couleur, la mode de filtrage, etc...

struct BitmapResult;

class GL33_Texture final {
public:
  GL33_Texture()=default;

  //retourne 0 pour pas d'erreur et -1 pour erreur
  int GL33_Create(const char* _imageContent, const size_t _imageSize);
  int GL33_CreateFromBitmap(BitmapResult* bmp);

  ~GL33_Texture();

  HRL_uint GetWidth() const;
  HRL_uint GetHeight() const;

  GLuint GetGL_ID() const;

  void Resize(HRL_uint width, HRL_uint height);

private:
  GLuint glID_;
  GLint width_, height_, nr_channels_;
};

#endif