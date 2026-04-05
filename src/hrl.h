/**
 * Copyright (c) 2025-2026 Oscar Soirey
 * https://github.com/oscar-soirey/Horizon-Rendering-Library
 *
 * This project was developed by a single passionate developer.
 * I ve tried to make everything work smoothly, but there may still be bugs.
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

#ifndef HRL_IMPL
#define HRL_IMPL

#define HRL_API_VERSION "0.3"

#ifdef __cplusplus
 #include <cstdint>
 #include <iostream>
#else
 #include <stdint.h>
 #include <stdio.h>
#endif


//windows dll compatibility

#ifdef _WIN32
	#ifdef HRL_BUILD_DLL
		#define HRL_API __declspec(dllexport)   //compilation de la DLL
	#elifdef HRL_NO_DLL
		#define HRL_API													//static
	#else
		#define HRL_API __declspec(dllimport)   //utilisation de la DLL
	#endif
#else
	#define HRL_API
#endif


//ajouter des debug views (genre activer que l'albedo ou que la normal, montrer la lumiere, les reflections, etc)
//suport multiviewport (avec HRL_CreateViewport et tout) (toujours 1 camera de rendu par viewport)

typedef uint32_t HRL_id;
typedef unsigned int HRL_uint;

//Bool
#define HRL_False ((int)0)
#define HRL_True  ((int)1)

//Errors
#define HRL_InvalidID										((HRL_id)-1)

//Textures
#define HRL_T_Albedo									"T_Albedo"
#define HRL_T_Normal									"T_Normal"
#define HRL_T_Specular								"T_Specular"
#define HRL_T_Roughness								"T_Roughness"
#define HRL_T_Metalic									"T_Metalic"
#define HRL_T_Alpha										"T_Alpha"
#define HRL_T_ShadowMap								"T_ShadowMap"
#define HRL_T_CubeMap									"T_CubeMap"

//APIs
#define HRL_OpenGL33										0x0001
#define HRL_OpenGL45										0x0002
#define HRL_Vulkan											0x0003
#define HRL_D3D11												0x0004
#define HRL_D3D12												0x0005
#define HRL_Metal												0x0006
#define HRL_NVN													0x0007
#define HRL_GNM													0x0008

//Lights
#define HRL_PointLight									0x0011
#define HRL_DirectionalLight						0x0012
#define HRL_SpotLight										0x0013

//Meshes & sprite
#define HRL_Sprite											0x0021   //[[deprecated]] in API
#define HRL_2D_Mesh											0x0022
#define HRL_3D_Mesh											0x0023
#define HRL_3D_SkeletalMesh							0x0024

//Debug geometry
#define HRL_DebugHollow									0x0031
#define HRL_DebugSolid									0x0032

//Camera
#define HRL_Ortho												0x0041
#define HRL_Perspective									0x0042

//Filter Textures
#define HRL_Filter_Nearest							0x0050
#define HRL_Filter_Linear								0x0051
#define HRL_Filter_Bilinear							0x0052
#define HRL_Filter_Trilinear						0x0053
#define HRL_Filter_Anisotropic					0x0054
#define HRL_Filter_Supersampling				0x0055

//Debug Views
#define HRL_DebugViewNone								0x0060
#define HRL_DebugViewNormal							0x0061
#define HRL_DebugViewLights							0x0062




//Default Shaders (we reserve theses ID)
#define HRL_SpriteShader								(UINT32_MAX)
#define HRL_Mesh2DShader								(UINT32_MAX - 1)
#define HRL_Mesh3DShader								(UINT32_MAX - 2)
#define HRL_DebugShader									(UINT32_MAX - 3)


#ifdef __cplusplus
extern "C" {
#endif

	//HRL Init
	HRL_API void HRL_Init(HRL_uint _api);
	HRL_API void HRL_InitContext(HRL_uint _width, HRL_uint _height, void* _loader);
	HRL_API void HRL_Shutdown();

	//HRL Frame
	HRL_API void HRL_BeginFrame();
	HRL_API void HRL_EndFrame();

	//Window
	/**
	 * Call this function when window is resized
	 */
	HRL_API void HRL_WindowResizeCallback(int _width, int _height);


	//Errors
	HRL_API const char* HRL_GetLastError();


	//HRL Meshes
	/**
	 * @brief Creates a new sprite in the given scene.
	 * @param _sceneid ID of the scene to add the sprite to.
	 * @return HRL_id of the new sprite object.
	 */
	HRL_API HRL_id HRL_CreateMeshSprite(HRL_id _sceneid);

	/**
	 * @brief Sets the pivot point of a sprite.
	 *
	 * Coordinates are normalized relative to the sprite:
	 * (-0.5,-0.5) = top-left, (0.5,0.5) = bottom-right, (0,0) = center.
	 *
	 * @param _meshid ID of the sprite.
	 * @param x Normalized horizontal pivot.
	 * @param y Normalized vertical pivot.
	 */
	HRL_API void HRL_SetMeshPivotPoint(HRL_id _meshid, float x, float y, float z);

	/**
	 * @brief Sets the UV coordinates for a sprite’s texture.
	 *
	 * @param _meshid ID of the sprite.
	 * @param min_u Minimum U coordinate (left).
	 * @param min_v Minimum V coordinate (top).
	 * @param max_u Maximum U coordinate (right).
	 * @param max_v Maximum V coordinate (bottom).
	 */
	HRL_API void HRL_SetSpriteRegion(HRL_id _meshid, float min_u, float min_v, float max_u, float max_v);

	/**
	 * @param _sceneid
	 * @param _type HRL_2D_Mesh, HRL_3D_Mesh, HRL_3D_SkeletalMesh
	 */
	HRL_API HRL_id HRL_CreateMesh(HRL_id _sceneid, HRL_uint _type, float* _vertices);

	/**
	 * Supported extensions : fbx, obj
	 * @return
	 */
	HRL_API HRL_id HRL_CreateMeshFromFile(HRL_id _sceneid, HRL_uint _type, const char* _data, size_t _bufferSize);

	/**
	 * @param _meshid HRL_id of the mesh
	 */
	HRL_API void HRL_DeleteMesh(HRL_id _meshid);

	/**
	 * @param _meshid HRL_id of the mesh
	 * @param _matid HRL_id of the mat
	 */
	HRL_API void HRL_SetMeshMaterial(HRL_id _meshid, HRL_id _matid);
	HRL_API void HRL_SetMeshLocation(HRL_id _meshid, float x, float y, float z);
	HRL_API void HRL_SetMeshRotation(HRL_id _meshid, float pitch, float yaw, float roll);
	HRL_API void HRL_SetMeshScale(HRL_id _meshid, float x, float y, float z);
	/**
	 * This will be used only when 2 sprites are collinding in Z axle
	 */
	HRL_API void HRL_SetSpriteDrawOrder(HRL_id _meshid, float _draworder);


	//Lights
	/**
	 * @param _type HRL_PointLight, HRL_DirectionalLight, HRL_SpotLight
	 * @return HRL_id of the new object
	 */
	HRL_API HRL_id HRL_CreateLight(HRL_id _sceneid, HRL_uint _type);
	/**
	 * @param _meshid HRL_id of the light
	 */
	HRL_API void HRL_DeleteLight(HRL_id _lightid);

	HRL_API void HRL_SetLightColor(HRL_id _lightid, float x, float y, float z);
	HRL_API void HRL_SetLightIntensity(HRL_id _lightid, float i);
	HRL_API void HRL_SetLightAttenuation(HRL_id _lightid, float a);

	HRL_API void HRL_SetLightLocation(HRL_id _lightid, float x, float y, float z);
	HRL_API void HRL_SetLightRotation(HRL_id _lightid, float pitch, float yaw, float roll);



	//Textures
	/**
	 * @brief Supported extensions : png, jpeg, jpg, bmp, tga, gif (single image), hdr, psd (half)
	 * To have more information, you can visit the stb website
	 * @param _data content of the file (supported extensions) (opened in binary mode)
	 * @return HRL_id of the new object
	 */
	HRL_API HRL_id HRL_CreateTexture(const char* _data, size_t _bufferSize);
	/**
	 * @param _textureid HRL_id of the texture
	 */
	HRL_API void HRL_DeleteTexture(HRL_id _textureid);

	/**
	 * @brief Set texture size
	 * @param _textureid
	 * @param width 
	 * @param height 
	 */
	HRL_API void HRL_ResizeTexture(HRL_id _textureid, int width, int height);

	/**
	 * @param _textureid Texture created by HRL_GenerateTextureFromText
	 * @param _width, _height Output dimensions in pixels
	 */
	HRL_API void HRL_GetTextureSize(HRL_id _textureid, int* _width, int* _height);

	//when the texture is smaller on the screen than its real size
	HRL_API void HRL_SetTextureMinFilter(HRL_id _textureid, HRL_uint _filter);
	//when the texture is bigger on the screen than its real size
	HRL_API void HRL_SetTextureMagFilter(HRL_id _textureid, HRL_uint _filter);

	/**
	 * @param _text Text input
	 * @param _fontid Font id (created with HRL_CreateFont)
	 * @param _font_size Vertical size (in pixel)
	 * @param _wrap_with Size (in pixel) before text wrap, 0 = no wrap
	 * @param r,g,b Text color
	 * @param bg_r, bg_g, bg_b Color of the background. bg_a (alpha), 0 means transparent
	 * @return id of the texture
	 */
	HRL_API HRL_id HRL_CreateTextureFromText(const char* _text, HRL_id _fontid,
		float _font_size, float _wrap_width,
		float r, float g, float b,
		float bg_r, float bg_g, float bg_b, float bg_a
	);


	//scenes
	/**
	 * @param _renderOnScreen render scene directely on the screen or render in a texture buffer.
	 * Default texture buffer size : 480x480, call ResizeSceneTexture to resize
	 */
	HRL_API HRL_id HRL_CreateScene(int _renderOnScreen);
	HRL_API void HRL_DeleteScene(HRL_id _sceneid);
	HRL_API void HRL_ResizeSceneTexture(HRL_id _sceneid, int _width, int _height);

	/**
	 * Util for color picking gpu
	 * @param _enable 0 for false, 1 for true.
	 */
	HRL_API void HRL_EnableColorPickingBuffer(HRL_id _scene, HRL_uint _enable);


	//Post Process
	HRL_API HRL_id HRL_CreatePostProcess(HRL_id _sceneid, HRL_id _matid);
	HRL_API void HRL_DeletePostProcess(HRL_id _postid);


	//Shaders
	HRL_API HRL_id HRL_CreateShader(const char* _vertData, size_t _vertSize, const char* _fragData, size_t _fragSize);
	HRL_API void HRL_DeleteShader(HRL_id _shaderid);


	//Materials
	//Wich contains values used by the source shader
	/**
	 * @return HRL_id of the new object
	 */
	HRL_API HRL_id HRL_CreateMaterial(HRL_id _shaderid);

	/**
	 * @param _meshid HRL_id of the light
	 */
	HRL_API void HRL_DeleteMaterial(HRL_id _matid);

	HRL_API void HRL_MaterialSetInt(HRL_id _matid, const char* _uniformName, int a);
	HRL_API void HRL_MaterialSetTexture(HRL_id _matid, const char* _uniformName, HRL_id _textureid);
	HRL_API void HRL_MaterialSetBool(HRL_id _matid, const char* _uniformName, int a);
	HRL_API void HRL_MaterialSetFloat(HRL_id _matid, const char* _uniformName, float a);
	HRL_API void HRL_MaterialSetVec2(HRL_id _matid, const char* _uniformName, float x, float y);
	HRL_API void HRL_MaterialSetVec3(HRL_id _matid, const char* _uniformName, float x, float y, float z);
	HRL_API void HRL_MaterialSetVec4(HRL_id _matid, const char* _uniformName, float x, float y, float z, float w);


	//Viewport
	/**
	 * @brief Allow to render world multiple times at the screen at same time.
	 * (Usefull for make splitscreen games for example).
	 * Normalized coordinates [0 - 1].
	 * [0,0] ----- [1,0]
	 * [0,1] ----- [1,1]
	 * @param _cameraid Can be HRL_InvalidID
	 */
	HRL_API HRL_id HRL_CreateViewport(HRL_id _sceneid, HRL_id _cameraid, float x, float y, float _width, float _height);
	HRL_API void HRL_DeleteViewport(HRL_id _viewportid);
	HRL_API void HRL_SetViewportCamera(HRL_id _viewportid, HRL_id _camid);
	HRL_API void HRL_SetViewportRect(HRL_id _viewportid, float x, float y, float _width, float _height);


	//Camera
	/**
	 * @brief Default type = ortho
	 * @param _type HRL_Ortho, HRL_Perspective
	 */
	HRL_API HRL_id HRL_CreateCamera(HRL_id _sceneid, HRL_uint _type);
	HRL_API void HRL_DeleteCamera(HRL_id _camid);

	HRL_API void HRL_SetCameraType(HRL_id _camid, HRL_uint _type);
	HRL_API void HRL_SetCameraOrthoVertical(HRL_id _camid, float _height);
	HRL_API void HRL_SetCameraPerspectiveFov(HRL_id _camid, float _fov);

	HRL_API void HRL_SetCameraNearPlane(HRL_id _camid, float _nearPlane);
	HRL_API void HRL_SetCameraFarPlane(HRL_id _camid, float _farPlane);

	HRL_API void HRL_SetCameraPosition(HRL_id _camid, float x, float y, float z);
	/**
	 * Backend information :
	 * Pitch : X, Yaw : Y, Roll : Z
	 */
	HRL_API void HRL_SetCameraRotation(HRL_id _camid, float pitch, float yaw, float roll);


	//Matrices
	/**
	 * @param aa Mat 4x4 colum-major (contiguous)
	 */
	HRL_API void HRL_GetProjectionMatrix(float* aa);
	HRL_API void HRL_GetViewMatrix(float* aa);
	HRL_API void HRL_GetModelMatrix(HRL_id _meshid, float* aa);


	//Debug views//
	/**
	 * @brief Draw the scene as debug, usefull to show normals, lights, ...
	 * @param mode HRL_DebugViewNone, HRL_DebugViewNormal, HRL_DebugViewLights,
	 */
	HRL_API void HRL_DrawSceneAsDebugMode(HRL_id _sceneid, HRL_uint mode);


	//Debug shapes
	//Rach functions need to be called every frame to render continuously

	//The value used by backend to define the thickness
	HRL_API void HRL_SetDebugLineThickness(float a);

	/**
	 * @param _sceneid Scene where to display debug draw
	 * @param a_x, a_y, a_z Coordinates of the start
	 * @param b_x, b_y, b_z Coordinates of the end
	 * @param r, g, b Color
	 */
	HRL_API void HRL_DrawDebugSegment(HRL_id _sceneid,
		float a_x, float a_y, float a_z,
		float b_x, float b_y, float b_z,
		float r, float g, float b
	);

	HRL_API void HRL_DrawDebugPolygon(HRL_id _sceneid, HRL_uint _mode,
		const float* vertices_x, const float* vertices_y, const float* vertices_z,
		int vertices_count,
		float r, float g, float b
	);

	HRL_API void HRL_DrawDebugCircle(HRL_id _sceneid, HRL_uint _mode,
		float center_x, float center_y, float center_z,
		float radius, int segments,
		float r, float g, float b
	);

	/**
	 * @param A Start
	 * @param B End
	 * @param _mode HRL_DebugHollow or HRL_DebugSolid
	 */
	HRL_API void HRL_DrawDebugCapsule(HRL_id _sceneid, HRL_uint _mode,
		float a_x, float a_y, float a_z,
		float b_x, float b_y, float b_z,
		float radius, int segments,
		float r, float g, float b
	);

	HRL_API void HRL_DrawDebugPoint(HRL_id _sceneid,
		float a_x, float a_y, float a_z,
		float size,
		float r, float g, float b
	);


	//Utility//
	/**
	 * @brief Take a screenshot of the target scene and save it as png image
	 * @param _target_path Absolute path where to save image
	 */
	HRL_API void HRL_TakeScreenshot(HRL_id _sceneid, const char* _target_path);

	/**
	 * @param _mode
	 */
	HRL_API void HRL_SetAntialiasingMode(HRL_uint _mode);


	//Optimizations
	/**
	 * @brief HRL will automatically resize the textures based on the distance with the camera and the texture
	 * @param _enable 0 means disable, 1 means enable
	 */
	HRL_API void HRL_EnableTextureAutoResize(HRL_uint _enable);




	//Text
	/**
	 * @param data Data of a ttf file
	 * @param _data_size file size in bytes
	 * @return New HRL id for the font
	 */
	HRL_API HRL_id HRL_CreateFont(const char* data, size_t _data_size);
	HRL_API void HRL_DeleteFont(HRL_id _fontid);



#ifdef __cplusplus
} //extern "C"
#endif


//macros
#ifdef __cplusplus  // C++
 #define HRL_CheckErrors() \
	std::cout << HRL_GetLastError() << ", " << __FILE__ << ", " << __LINE__ << std::endl

#else // C
#define HRL_CheckErrors() \
	printf("%s, %s, %d\n", HRL_GetLastError(), __FILE__, __LINE__)

#endif


#endif //HRL_IMPL