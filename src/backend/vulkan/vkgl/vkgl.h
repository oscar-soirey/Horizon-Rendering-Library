// Generated: OpenGL 3.3 entry points used by the HRL renderer, implemented
// over Vulkan (see vkgl.cpp). Included instead of the glad function table.
#ifndef HRL_VKGL_H
#define HRL_VKGL_H
#include <glad/glad.h>

#undef glActiveTexture
#define glActiveTexture vkgl_ActiveTexture
void vkgl_ActiveTexture(GLenum texture);
#undef glAttachShader
#define glAttachShader vkgl_AttachShader
void vkgl_AttachShader(GLuint program, GLuint shader);
#undef glBindBuffer
#define glBindBuffer vkgl_BindBuffer
void vkgl_BindBuffer(GLenum target, GLuint buffer);
#undef glBindBufferBase
#define glBindBufferBase vkgl_BindBufferBase
void vkgl_BindBufferBase(GLenum target, GLuint index, GLuint buffer);
#undef glBindFramebuffer
#define glBindFramebuffer vkgl_BindFramebuffer
void vkgl_BindFramebuffer(GLenum target, GLuint framebuffer);
#undef glBindRenderbuffer
#define glBindRenderbuffer vkgl_BindRenderbuffer
void vkgl_BindRenderbuffer(GLenum target, GLuint renderbuffer);
#undef glBindSampler
#define glBindSampler vkgl_BindSampler
void vkgl_BindSampler(GLuint unit, GLuint sampler);
#undef glBindTexture
#define glBindTexture vkgl_BindTexture
void vkgl_BindTexture(GLenum target, GLuint texture);
#undef glBindVertexArray
#define glBindVertexArray vkgl_BindVertexArray
void vkgl_BindVertexArray(GLuint array);
#undef glBlendFunc
#define glBlendFunc vkgl_BlendFunc
void vkgl_BlendFunc(GLenum sfactor, GLenum dfactor);
#undef glBlitFramebuffer
#define glBlitFramebuffer vkgl_BlitFramebuffer
void vkgl_BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
#undef glBufferData
#define glBufferData vkgl_BufferData
void vkgl_BufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
#undef glBufferSubData
#define glBufferSubData vkgl_BufferSubData
void vkgl_BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
#undef glCheckFramebufferStatus
#define glCheckFramebufferStatus vkgl_CheckFramebufferStatus
GLenum vkgl_CheckFramebufferStatus(GLenum target);
#undef glClear
#define glClear vkgl_Clear
void vkgl_Clear(GLbitfield mask);
#undef glClearBufferfv
#define glClearBufferfv vkgl_ClearBufferfv
void vkgl_ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat *value);
#undef glClearColor
#define glClearColor vkgl_ClearColor
void vkgl_ClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
#undef glColorMaski
#define glColorMaski vkgl_ColorMaski
void vkgl_ColorMaski(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a);
#undef glCompileShader
#define glCompileShader vkgl_CompileShader
void vkgl_CompileShader(GLuint shader);
#undef glCopyTexSubImage2D
#define glCopyTexSubImage2D vkgl_CopyTexSubImage2D
void vkgl_CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
#undef glCreateProgram
#define glCreateProgram vkgl_CreateProgram
GLuint vkgl_CreateProgram(void);
#undef glCreateShader
#define glCreateShader vkgl_CreateShader
GLuint vkgl_CreateShader(GLenum type);
#undef glCullFace
#define glCullFace vkgl_CullFace
void vkgl_CullFace(GLenum mode);
#undef glDeleteBuffers
#define glDeleteBuffers vkgl_DeleteBuffers
void vkgl_DeleteBuffers(GLsizei n, const GLuint *buffers);
#undef glDeleteFramebuffers
#define glDeleteFramebuffers vkgl_DeleteFramebuffers
void vkgl_DeleteFramebuffers(GLsizei n, const GLuint *framebuffers);
#undef glDeleteProgram
#define glDeleteProgram vkgl_DeleteProgram
void vkgl_DeleteProgram(GLuint program);
#undef glDeleteRenderbuffers
#define glDeleteRenderbuffers vkgl_DeleteRenderbuffers
void vkgl_DeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers);
#undef glDeleteShader
#define glDeleteShader vkgl_DeleteShader
void vkgl_DeleteShader(GLuint shader);
#undef glDeleteTextures
#define glDeleteTextures vkgl_DeleteTextures
void vkgl_DeleteTextures(GLsizei n, const GLuint *textures);
#undef glDeleteVertexArrays
#define glDeleteVertexArrays vkgl_DeleteVertexArrays
void vkgl_DeleteVertexArrays(GLsizei n, const GLuint *arrays);
#undef glDeleteSamplers
#define glDeleteSamplers vkgl_DeleteSamplers
void vkgl_DeleteSamplers(GLsizei count, const GLuint *samplers);
#undef glDepthFunc
#define glDepthFunc vkgl_DepthFunc
void vkgl_DepthFunc(GLenum func);
#undef glDepthMask
#define glDepthMask vkgl_DepthMask
void vkgl_DepthMask(GLboolean flag);
#undef glDisable
#define glDisable vkgl_Disable
void vkgl_Disable(GLenum cap);
#undef glDisableVertexAttribArray
#define glDisableVertexAttribArray vkgl_DisableVertexAttribArray
void vkgl_DisableVertexAttribArray(GLuint index);
#undef glDrawArrays
#define glDrawArrays vkgl_DrawArrays
void vkgl_DrawArrays(GLenum mode, GLint first, GLsizei count);
#undef glDrawBuffer
#define glDrawBuffer vkgl_DrawBuffer
void vkgl_DrawBuffer(GLenum buf);
#undef glDrawBuffers
#define glDrawBuffers vkgl_DrawBuffers
void vkgl_DrawBuffers(GLsizei n, const GLenum *bufs);
#undef glDrawElements
#define glDrawElements vkgl_DrawElements
void vkgl_DrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices);
#undef glDrawElementsInstanced
#define glDrawElementsInstanced vkgl_DrawElementsInstanced
void vkgl_DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount);
#undef glEnable
#define glEnable vkgl_Enable
void vkgl_Enable(GLenum cap);
#undef glEnableVertexAttribArray
#define glEnableVertexAttribArray vkgl_EnableVertexAttribArray
void vkgl_EnableVertexAttribArray(GLuint index);
#undef glFramebufferRenderbuffer
#define glFramebufferRenderbuffer vkgl_FramebufferRenderbuffer
void vkgl_FramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
#undef glFramebufferTexture2D
#define glFramebufferTexture2D vkgl_FramebufferTexture2D
void vkgl_FramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
#undef glFramebufferTextureLayer
#define glFramebufferTextureLayer vkgl_FramebufferTextureLayer
void vkgl_FramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer);
#undef glGenBuffers
#define glGenBuffers vkgl_GenBuffers
void vkgl_GenBuffers(GLsizei n, GLuint *buffers);
#undef glGenFramebuffers
#define glGenFramebuffers vkgl_GenFramebuffers
void vkgl_GenFramebuffers(GLsizei n, GLuint *framebuffers);
#undef glGenRenderbuffers
#define glGenRenderbuffers vkgl_GenRenderbuffers
void vkgl_GenRenderbuffers(GLsizei n, GLuint *renderbuffers);
#undef glGenSamplers
#define glGenSamplers vkgl_GenSamplers
void vkgl_GenSamplers(GLsizei count, GLuint *samplers);
#undef glGenTextures
#define glGenTextures vkgl_GenTextures
void vkgl_GenTextures(GLsizei n, GLuint *textures);
#undef glGenVertexArrays
#define glGenVertexArrays vkgl_GenVertexArrays
void vkgl_GenVertexArrays(GLsizei n, GLuint *arrays);
#undef glGenerateMipmap
#define glGenerateMipmap vkgl_GenerateMipmap
void vkgl_GenerateMipmap(GLenum target);
#undef glGetIntegerv
#define glGetIntegerv vkgl_GetIntegerv
void vkgl_GetIntegerv(GLenum pname, GLint *data);
#undef glGetError
#define glGetError vkgl_GetError
GLenum vkgl_GetError(void);
#undef glGetProgramInfoLog
#define glGetProgramInfoLog vkgl_GetProgramInfoLog
void vkgl_GetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
#undef glGetProgramiv
#define glGetProgramiv vkgl_GetProgramiv
void vkgl_GetProgramiv(GLuint program, GLenum pname, GLint *params);
#undef glGetShaderInfoLog
#define glGetShaderInfoLog vkgl_GetShaderInfoLog
void vkgl_GetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
#undef glGetShaderiv
#define glGetShaderiv vkgl_GetShaderiv
void vkgl_GetShaderiv(GLuint shader, GLenum pname, GLint *params);
#undef glGetUniformBlockIndex
#define glGetUniformBlockIndex vkgl_GetUniformBlockIndex
GLuint vkgl_GetUniformBlockIndex(GLuint program, const GLchar *uniformBlockName);
#undef glGetUniformLocation
#define glGetUniformLocation vkgl_GetUniformLocation
GLint vkgl_GetUniformLocation(GLuint program, const GLchar *name);
#undef glIsEnabled
#define glIsEnabled vkgl_IsEnabled
GLboolean vkgl_IsEnabled(GLenum cap);
#undef glLineWidth
#define glLineWidth vkgl_LineWidth
void vkgl_LineWidth(GLfloat width);
#undef glLinkProgram
#define glLinkProgram vkgl_LinkProgram
void vkgl_LinkProgram(GLuint program);
#undef glPixelStorei
#define glPixelStorei vkgl_PixelStorei
void vkgl_PixelStorei(GLenum pname, GLint param);
#undef glPolygonMode
#define glPolygonMode vkgl_PolygonMode
void vkgl_PolygonMode(GLenum face, GLenum mode);
#undef glPolygonOffset
#define glPolygonOffset vkgl_PolygonOffset
void vkgl_PolygonOffset(GLfloat factor, GLfloat units);
#undef glReadBuffer
#define glReadBuffer vkgl_ReadBuffer
void vkgl_ReadBuffer(GLenum src);
#undef glReadPixels
#define glReadPixels vkgl_ReadPixels
void vkgl_ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels);
#undef glRenderbufferStorage
#define glRenderbufferStorage vkgl_RenderbufferStorage
void vkgl_RenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
#undef glRenderbufferStorageMultisample
#define glRenderbufferStorageMultisample vkgl_RenderbufferStorageMultisample
void vkgl_RenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height);
#undef glSamplerParameteri
#define glSamplerParameteri vkgl_SamplerParameteri
void vkgl_SamplerParameteri(GLuint sampler, GLenum pname, GLint param);
#undef glShaderSource
#define glShaderSource vkgl_ShaderSource
void vkgl_ShaderSource(GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length);
#undef glTexImage2D
#define glTexImage2D vkgl_TexImage2D
void vkgl_TexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels);
#undef glTexImage2DMultisample
#define glTexImage2DMultisample vkgl_TexImage2DMultisample
void vkgl_TexImage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations);
#undef glTexImage3D
#define glTexImage3D vkgl_TexImage3D
void vkgl_TexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels);
#undef glTexParameterfv
#define glTexParameterfv vkgl_TexParameterfv
void vkgl_TexParameterfv(GLenum target, GLenum pname, const GLfloat *params);
#undef glTexParameteri
#define glTexParameteri vkgl_TexParameteri
void vkgl_TexParameteri(GLenum target, GLenum pname, GLint param);
#undef glTexSubImage2D
#define glTexSubImage2D vkgl_TexSubImage2D
void vkgl_TexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels);
#undef glTexSubImage3D
#define glTexSubImage3D vkgl_TexSubImage3D
void vkgl_TexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void *pixels);
#undef glUniform1f
#define glUniform1f vkgl_Uniform1f
void vkgl_Uniform1f(GLint location, GLfloat v0);
#undef glUniform1i
#define glUniform1i vkgl_Uniform1i
void vkgl_Uniform1i(GLint location, GLint v0);
#undef glUniform1ui
#define glUniform1ui vkgl_Uniform1ui
void vkgl_Uniform1ui(GLint location, GLuint v0);
#undef glUniform2f
#define glUniform2f vkgl_Uniform2f
void vkgl_Uniform2f(GLint location, GLfloat v0, GLfloat v1);
#undef glUniform3f
#define glUniform3f vkgl_Uniform3f
void vkgl_Uniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
#undef glUniform4f
#define glUniform4f vkgl_Uniform4f
void vkgl_Uniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
#undef glUniform1fv
#define glUniform1fv vkgl_Uniform1fv
void vkgl_Uniform1fv(GLint location, GLsizei count, const GLfloat *value);
#undef glUniform2fv
#define glUniform2fv vkgl_Uniform2fv
void vkgl_Uniform2fv(GLint location, GLsizei count, const GLfloat *value);
#undef glUniform3fv
#define glUniform3fv vkgl_Uniform3fv
void vkgl_Uniform3fv(GLint location, GLsizei count, const GLfloat *value);
#undef glUniform4fv
#define glUniform4fv vkgl_Uniform4fv
void vkgl_Uniform4fv(GLint location, GLsizei count, const GLfloat *value);
#undef glUniform1iv
#define glUniform1iv vkgl_Uniform1iv
void vkgl_Uniform1iv(GLint location, GLsizei count, const GLint *value);
#undef glUniformMatrix3fv
#define glUniformMatrix3fv vkgl_UniformMatrix3fv
void vkgl_UniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
#undef glUniformMatrix4fv
#define glUniformMatrix4fv vkgl_UniformMatrix4fv
void vkgl_UniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
#undef glUniformBlockBinding
#define glUniformBlockBinding vkgl_UniformBlockBinding
void vkgl_UniformBlockBinding(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding);
#undef glUseProgram
#define glUseProgram vkgl_UseProgram
void vkgl_UseProgram(GLuint program);
#undef glVertexAttrib3f
#define glVertexAttrib3f vkgl_VertexAttrib3f
void vkgl_VertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z);
#undef glVertexAttrib4f
#define glVertexAttrib4f vkgl_VertexAttrib4f
void vkgl_VertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
#undef glVertexAttribDivisor
#define glVertexAttribDivisor vkgl_VertexAttribDivisor
void vkgl_VertexAttribDivisor(GLuint index, GLuint divisor);
#undef glVertexAttribIPointer
#define glVertexAttribIPointer vkgl_VertexAttribIPointer
void vkgl_VertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void *pointer);
#undef glVertexAttribPointer
#define glVertexAttribPointer vkgl_VertexAttribPointer
void vkgl_VertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);
#undef glViewport
#define glViewport vkgl_Viewport
void vkgl_Viewport(GLint x, GLint y, GLsizei width, GLsizei height);
#undef glScissor
#define glScissor vkgl_Scissor
void vkgl_Scissor(GLint x, GLint y, GLsizei width, GLsizei height);
#undef glFinish
#define glFinish vkgl_Finish
void vkgl_Finish(void);
#undef glFlush
#define glFlush vkgl_Flush
void vkgl_Flush(void);
#undef glBlendFuncSeparate
#define glBlendFuncSeparate vkgl_BlendFuncSeparate
void vkgl_BlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha);
#undef glBlendEquation
#define glBlendEquation vkgl_BlendEquation
void vkgl_BlendEquation(GLenum mode);
#undef gladLoadGLLoader
#define gladLoadGLLoader vkgl_LoadLoader
int vkgl_LoadLoader(GLADloadproc);
#endif
