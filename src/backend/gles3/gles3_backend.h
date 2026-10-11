#ifndef HRL_GLES3_BACKEND
#define HRL_GLES3_BACKEND

#include "../../core/backend_vtable.h"

// OpenGL ES 3.0 backend (HRL_OPENGL_ES_30). Compiled when HRL_ENABLE_GLES3 is defined.
HRL_vtable GetOpenGLES3Backend();

/// The framebuffer on-screen scenes are drawn into (default: 0, the window).
/// Behind HRL_SetDefaultFramebuffer; iOS needs it.
void GLES3_SetDefaultFramebuffer(unsigned int framebuffer);

#endif
