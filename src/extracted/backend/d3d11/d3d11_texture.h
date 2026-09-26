#ifndef D3D11_TEXTURE
#define D3D11_TEXTURE

#include "../../hrl.h"
#include <glad/glad.h>

//a l'avenir, donner plus de possibilités aux textures, changer l'encodage couleur, la mode de filtrage, etc...

class D3D11_Texture final {
public:
  D3D11_Texture()=default;

  //retourne 0 pour pas d'erreur et -1 pour erreur
  int D3D11_Create(const HRL_uint _type, const char* _imageContent, const size_t _imageSize);

  ~D3D11_Texture();

  void Bind();
  HRL_uint GetType() const;

  HRL_uint GetWidth() const;
  HRL_uint GetHeight() const;

  void Resize(HRL_uint width, HRL_uint height);

private:
  GLuint d3dID_;
  GLint width_, height_, nr_channels_;

  HRL_uint type_;
};

#endif