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
 *
 *
 * This is official C++ API for Horizon-Rendering-Library
 * Features may difer between C++ and classic C API. Theses can be used together
 */
 
#ifndef HRLPP_IMPL
#define HRLPP_IMPL

#define HRL_API_VERSION "0.5"
#define HRL_CPP

#include <cstdint>
#include <cstddef>
#include <iostream>

#ifdef _WIN32
	#ifdef HRL_BUILD_DLL
		#define HRL_API __declspec(dllexport)
	#elif defined(HRL_NO_DLL)
		#define HRL_API
	#else
		#define HRL_API __declspec(dllimport)
	#endif
#else
	#define HRL_API
#endif

namespace hrl
{
	
	


	typedef uint32_t id;
	typedef uint32_t uint;

	/**
	 * @brief Vertex format used by HRL_CreateMesh3D.
	 *
	 * All vectors are expressed in object/local space. UV coordinates use the same
	 * convention as HRL_CreateMeshSprite and HRL_SetSpriteRegion. Tangent and
	 * bitangent are used with the normal map for the built-in OpenGL 3.3 shader.
	 */
	typedef struct Vertex3D {
		float position[3];
		float normal[3];
		float uv[2];
		float tangent[3];
		float bitangent[3];
	} Vertex3D;

	#define HRL_INVALID_ID								((HRL_id)-1)

	#define HRL_T_ALBEDO									"T_Albedo"
	#define HRL_T_NORMAL									"T_Normal"
	#define HRL_T_SPECULAR								"T_Specular"
	#define HRL_T_ROUGHNESS								"T_Roughness"
	#define HRL_T_METALLIC								"T_Metallic"
	#define HRL_T_AMBIENT_OCCLUSION				"T_AO"
	#define HRL_T_ALPHA										"T_Alpha"
	#define HRL_T_EMISSIVE								"T_Emissive"
	#define HRL_T_SHADOW_MAP							"T_ShadowMap"
	#define HRL_T_CUBE_MAP								"T_CubeMap"

	enum E_APIs{
		HRL_OPENGL_33 = 0x0001,
		HRL_OPENGL_45,
		HRL_VULKAN,
		HRL_D3D11,
		HRL_D3D12,
		HRL_METAL,
		HRL_NVN,
		HRL_GNM
	};

	enum ELightType{
		HRL_POINT_LIGHT = 0x0011,
		HRL_DIRECTIONAL_LIGHT,
		HRL_SPOT_LIGHT
	};

	enum EMeshType{
		HRL_SPRITE = 0x0021,
		HRL_2D_MESH,
		HRL_3D_MESH,
		HRL_3D_SKELETAL_MESH
	};

	enum EDebugRenderingType{
		HRL_DEBUG_HOLLOW = 0x0031,
		HRL_DEBUG_SOLID
	};

	enum ECameraType{
		HRL_ORTHO = 0x0041,
		HRL_PERSPECTIVE
	};

	enum EFilterType{
		HRL_FILTER_NEAREST = 0x0050,
		HRL_FILTER_LINEAR,
		HRL_FILTER_BILINEAR,
		HRL_FILTER_TRILINEAR,
		HRL_FILTER_ANISOTROPIC,
		/** Not avalaible with OpenGL backends */
		HRL_FILTER_SUPERSAMPLING
	};

	enum EAntialiasingMode{
		HRL_ANTIALIASING_OFF = 0,
		HRL_ANTIALIASING_2X = 2,
		HRL_ANTIALIASING_4X = 4,
		HRL_ANTIALIASING_8X = 8
	};

	enum EDebugView{
		HRL_DEBUG_VIEW_NONE = 0x0060,
		HRL_DEBUG_VIEW_UNLIT,
		HRL_DEBUG_VIEW_NORMAL,
		HRL_DEBUG_VIEW_LIGHTS,
		HRL_DEBUG_VIEW_LIGHTING = HRL_DEBUG_VIEW_LIGHTS,
		HRL_DEBUG_VIEW_WIREFRAME,
		HRL_DEBUG_VIEW_LOD
	};

	enum EError{
		HRL_NO_ERROR=0x0070,
		HRL_ERROR_INVALID_ID,
		HRL_INVALID_ENUM,
		HRL_INVALID_VALUE,
		HRL_INVALID_OPERATION,
		HRL_INVALID_BACKEND_OPERATION,
		HRL_SHADER_COMPILE_FAIL,
		HRL_OUT_OF_MEMORY,
		HRL_INVALID_FILE_FORMAT
	};
	enum ESeverity{
		HRL_SEVERITY_WEAK_WARNING=0x0080,
		HRL_SEVERITY_WARNING,
		HRL_SEVERITY_ERROR,
		HRL_SEVERITY_FATAL
	};

	enum EFogType{
		HRL_FOG_LINEAR = 0x0090,
		HRL_FOG_EXPONENTIAL,
		HRL_FOG_EXP_SQUARED
	};

	enum EWidgetState{
		HRL_WIDGET_STATE_IDLE = (1 << 0),
		HRL_WIDGET_STATE_HOVERED = (1 << 1),
		HRL_WIDGET_STATE_PRESSED = (1 << 2)
	};

	enum EWidgetType{
		HRL_WIDGET_BUTTON = 0x00A0,
		HRL_WIDGET_LABEL,
		HRL_WIDGET_IMAGE,
		HRL_WIDGET_SLIDER,
		HRL_WIDGET_CHECKBOX,
		HRL_WIDGET_PROGRESSBAR
	};


	/** Default Shaders (HRL reserve theses ID) */
	#define HRL_SPRITE_SHADER (UINT32_MAX)
	#define HRL_MESH_2D_SHADER (UINT32_MAX - 1)
	#define HRL_MESH_3D_SHADER (UINT32_MAX - 2)
	#define HRL_DEBUG_SHADER (UINT32_MAX - 3)
	#define HRL_DEFAULT_POST_PROCESS_SHADER (UINT32_MAX - 4)


	typedef void (*HRL_CErrorCallback)(EError code, ESeverity severity, const char* detail);

	class HRL_API Instance {
	public:
		//using HRL_Init & HRL_InitContext
		Instance(HRL_E_APIs _api, HRL_uint _width, HRL_uint _height, void* _loader);
		//using HRL_Shutdown
		~Instance();

		void BeginFrame();
		void EndFrame();

		//using HRL_WindowResizeCallback
		void WindowResizeCallback(int _width, int _height);



		/* ============================================================================
		 *  ERROR HANDLING
		 * ============================================================================ */
		EError GetLastError(const char** _detail, HRL_ESeverity* _severity);
		std::string ErrorEnumToString(EError err);
		std::string SeverityEnumToString(ESeverity sev);
		void RegisterErrorCallback(CErrorCallback _callback);
	};


	/* ============================================================================
	 *  MESHES & SPRITES
	 * ============================================================================ */

	class HRL_API Mesh {
	public:

	}



} //namespace hrl

#endif