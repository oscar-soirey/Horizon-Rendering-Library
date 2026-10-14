// Prelude of the translation units that compile the OpenGL 3.3 renderer for
// the Vulkan backend: OpenGL calls go to vkgl.cpp, global symbols are renamed.
#ifndef HRL_VKGL_PRELUDE_H
#define HRL_VKGL_PRELUDE_H
#include "vkgl.h"
#include "vkgl_rename.h"
#define STB_IMAGE_STATIC
#endif
