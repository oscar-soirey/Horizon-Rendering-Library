// HRL - OpenGL ES 3.0 backend: GL entry points.
//
// The backend reuses HRL's glad loader (the same one as the OpenGL 3.3
// backend) instead of the platform <GLES3/gl3.h> header: every function it
// calls exists with the same name, signature and enum values in OpenGL ES 3.0,
// and going through the loader passed to HRL_InitContext keeps the backend
// usable wherever an ES 3 context can be created (Android/EGL, desktop drivers
// exposing an ES profile, ANGLE...).
//
// Rule for this directory: only call functions that are part of OpenGL ES 3.0.
// (No glPolygonMode, glDrawBuffer, glGetTexImage, GL_MULTISAMPLE, ...)
#ifndef HRL_GLES3_GL_H
#define HRL_GLES3_GL_H

#include <glad/glad.h>

/// Loads the GL entry points through `loader` (a `void* (*)(const char*)`
/// such as eglGetProcAddress or glfwGetProcAddress). Returns false on failure.
bool GLES3_LoadGL(void* loader);

#endif
