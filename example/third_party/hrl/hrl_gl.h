/**
* Copyright (c) 2025-2026 Oscar Soirey
 * https://github.com/oscar-soirey/Horizon-Rendering-Library
 *
 * This project was developed by a single passionate developer.
 * I've tried to make everything work smoothly, but there may still be bugs.
 * If you encounter any issues or have suggestions, please feel free to contact me at:
 * oscarsoirey.contact@gmail.com
 * Thank you for your support and understanding
 *
 * This code is the intellectual property of Oscar Soirey and is
 * licensed under the Apache License, Version 2.0. You may not use,
 * modify, or distribute this software except in compliance with the
 * License. A copy of the License can be obtained at:
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * By using or modifying this code, you agree to adhere to the terms
 * of the Apache 2.0 License.
 *
 *    ,--.  ,--.,------. ,--.
 *    |  '--'  ||  .--. '|  |
 *    |  .--.  ||  '--'.'|  |
 *    |  |  |  ||  |\  \ |  '--.
 *    `--'  `--'`--' '--'`-----'
 */

#ifndef HRL_GL_IMPL
#define HRL_GL_IMPL

#include "hrl.h"

#ifdef __cplusplus
	extern "C" {
#endif

		/**
		 * @param _textureid HRL_id of the texture
		 * @return Returns the OpenGL backend id of the GPU texture
		 */
		HRL_API unsigned int HRL_GL_GetTextureGL_ID(HRL_id _textureid);

		/**
		 * @param _shaderid HRL_id of the shader
		 * @return Returns the OpenGL backend id of the shader program
		 */
		HRL_API unsigned int HRL_GL_GetShaderGL_ID(HRL_id _shaderid);

		HRL_API unsigned int HRL_GL_GetSceneTextureGL_ID(HRL_id _sceneid);

		HRL_API unsigned int HRL_GL_GetSceneColorBufferGL_ID(HRL_id _sceneid);
		HRL_API HRL_id HRL_GL_GetHoveredObject(HRL_id _scene, int mouseX, int mouseY, HRL_EMeshType* mesh_type);

#ifdef __cplusplus
	}
#endif

#endif