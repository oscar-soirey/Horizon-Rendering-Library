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

#ifdef __cplusplus
 #include <cstdint>
 #include <iostream>
#else
 #include <stdint.h>
 #include <stdio.h>
#endif


//compatibilité dll
//#define HRL_BUILD_DLL
//#define HRL_NO_DLL

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
#define HRL_Sprite											0x0021
#define HRL_2D_Mesh											0x0022
#define HRL_3D_Mesh											0x0023

//Debug geometry
//#define HRL_DebugLine										0x0031
//#define HRL_DebugBox										0x0032
//#define HRL_DebugSphere									0x0033

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
#define HRL_Filter_Anistropic						0x0054
#define HRL_Filter_Supersampling				0x0055




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
	 * @param _type HRL_Sprite, HRL_2D_Mesh, HRL_3D_Mesh
	 * @return HRL_id of the new object
	 */
	HRL_API HRL_id HRL_CreateMesh(HRL_id _sceneid, HRL_uint _type);
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
	//ajouter des fonctions de controle des textures

	//when the texture is smaller on the screen than its real size
	HRL_API void HRL_SetTextureMinFilter(HRL_uint _filter);
	//when the texture is bigger on the screen than its real size
	HRL_API void HRL_SetTextureMagFilter(HRL_uint _filter);


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
	HRL_API void HRL_EnableColorPickingBuffer(HRL_id _scene, int _enable);


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
	//HRL_API void HRL_GetProjectionMatrice(float* aa, ...);
	//HRL_API void HRL_GetViewMatrice(float* aa, ...);
	//HRL_API void HRL_GetModelMatrice(float* aa, ...);


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
		float radius, float segments,
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
		float radius, float segments,
		float r, float g, float b
	);

	HRL_API void HRL_DrawDebugPoint(HRL_id _sceneid,
		float a_x, float a_y, float a_z,
		float size,
		float r, float g, float b
	);


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