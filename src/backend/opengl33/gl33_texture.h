#ifndef GL33_TEXTURE
#define GL33_TEXTURE

#include "../../hrl.h"
#include <glad/glad.h>
#include <vector>

//a l'avenir, donner plus de possibilités aux textures, changer l'encodage couleur, la mode de filtrage, etc...

struct BitmapResult;

class GL33_Texture final {
public:
  GL33_Texture()=default;

  ~GL33_Texture();

  //retourne 0 pour pas d'erreur et -1 pour erreur
  int GL33_Create(const char* _imageContent, const size_t _imageSize);
  int GL33_CreateFromBitmap(BitmapResult* bmp);

  HRL_uint GetWidth() const;
  HRL_uint GetHeight() const;

  GLuint GetGL_ID() const;
  const std::vector<unsigned char>& GetCpuRGBA() const { return cpu_rgba_; }

  void SetMinFilter(HRL_uint filter);
  void SetMaxFilter(HRL_uint filter);


private:
  GLuint glID_ = 0;
  GLint width_ = 0, height_ = 0, nr_channels_ = 4;
  std::vector<unsigned char> cpu_rgba_;
  bool mipmaps_generated_ = false;
};

#endif