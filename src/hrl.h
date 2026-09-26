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

#define HRL_API_VERSION "0.6"

#ifdef __cplusplus
 #include <cstdint>
 #include <cstddef>
 #include <iostream>
#else
 #include <stdint.h>
 #include <stddef.h>
 #include <stdio.h>
#endif


/** windows dll compatibility */

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


typedef uint32_t HRL_id;
typedef uint32_t HRL_uint;

/**
 * @brief Vertex format used by HRL_CreateMesh3D.
 *
 * All vectors are expressed in object/local space. UV coordinates use the same
 * convention as HRL_CreateMeshSprite and HRL_SetSpriteRegion. Tangent and
 * bitangent are used with the normal map for the built-in OpenGL 3.3 shader.
 */
typedef struct HRL_Vertex3D {
	float position[3];
	float normal[3];
	float uv[2];
	float tangent[3];
	float bitangent[3];
} HRL_Vertex3D;

/** Maximum number of bone influences stored per skeletal vertex. */
#define HRL_SKELETAL_MAX_INFLUENCES 4
/** Maximum number of bones supported by the OpenGL 3.3 skeletal renderer. */
#define HRL_MAX_SKELETAL_BONES 128
/** Maximum number of localized volumetric fog objects rendered per scene. */
#define HRL_MAX_VOLUMETRIC_FOGS 64

/** Vertex format used by skeletal meshes. The first five attributes share the
 * HRL_Vertex3D layout, followed by four bone indices and four normalized weights. */
typedef struct HRL_SkeletalVertex {
	HRL_Vertex3D vertex;
	HRL_uint boneIndices[HRL_SKELETAL_MAX_INFLUENCES];
	float boneWeights[HRL_SKELETAL_MAX_INFLUENCES];
} HRL_SkeletalVertex;

/** Local transform sampled for one skeletal bone at one animation frame. */
typedef struct HRL_SkeletalBoneTransform {
	float translation[3];
	float rotation[4]; /* quaternion x, y, z, w */
	float scale[3];
} HRL_SkeletalBoneTransform;

/** Public skeletal bone metadata. */
typedef struct HRL_SkeletalBone {
	const char* name;
	HRL_uint parentIndex;
	/** Local bind-pose transform used to reconstruct the bone hierarchy. */
	HRL_SkeletalBoneTransform bindTransform;
	/** Evaluated bind-pose node-to-world matrix. */
	float bindWorldMatrix[16];
	float inverseBindMatrix[16];
} HRL_SkeletalBone;

/** Baked animation data owned by HRL_SkeletalMeshData until freed. */
typedef struct HRL_SkeletalAnimation {
	const char* name;
	float duration;
	float frameRate;
	size_t frameCount;
	HRL_SkeletalBoneTransform* frames; /* frameCount * boneCount */
	/** Optional evaluated skin matrices: frameCount * boneCount * 16 floats. */
	float* poseMatrices;
} HRL_SkeletalAnimation;

/** CPU-side skeletal mesh data produced by an importer. */
typedef struct HRL_SkeletalMeshData {
	HRL_SkeletalVertex* vertices;
	size_t vertexCount;
	HRL_uint* indices;
	size_t indexCount;
	HRL_SkeletalBone* bones;
	size_t boneCount;
	HRL_SkeletalAnimation* animations;
	size_t animationCount;
	/** Geometry-to-world transform of the FBX mesh instance. New data should set this to identity unless imported from FBX. */
	float geometryToWorldMatrix[16];
} HRL_SkeletalMeshData;

#define HRL_FALSE ((int)0)
#define HRL_TRUE  ((int)1)

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

typedef enum HRL_E_APIs{
	HRL_OPENGL_33 = 0x0001,
	HRL_OPENGL_45,
	HRL_VULKAN,
	HRL_D3D11,
	HRL_D3D12,
	HRL_METAL,
	HRL_NVN,
	HRL_GNM
}HRL_E_APIs;

typedef enum HRL_ELightType{
	HRL_POINT_LIGHT = 0x0011,
	HRL_DIRECTIONAL_LIGHT,
	HRL_SPOT_LIGHT
}HRL_ELightType;

typedef enum HRL_EMeshType{
	HRL_SPRITE = 0x0021,
	HRL_2D_MESH,
	HRL_3D_MESH,
	HRL_3D_SKELETAL_MESH
}HRL_EMeshType;

typedef enum HRL_EDebugRenderingType{
	HRL_DEBUG_HOLLOW = 0x0031,
	HRL_DEBUG_SOLID
}HRL_EDebugRenderingType;

typedef enum HRL_ECameraType{
	HRL_ORTHO = 0x0041,
	HRL_PERSPECTIVE
}HRL_ECameraType;

typedef enum HRL_EFilterType{
	HRL_FILTER_NEAREST = 0x0050,
	HRL_FILTER_LINEAR,
	HRL_FILTER_BILINEAR,
	HRL_FILTER_TRILINEAR,
	HRL_FILTER_ANISOTROPIC,
	/** Not avalaible with OpenGL backends */
	HRL_FILTER_SUPERSAMPLING
}HRL_EFilterType;

/**
 * Global illumination methods.
 *
 * Backend support in this release:
 *   - OpenGL 3.3: DDGI-style world-space irradiance probes
 *   - OpenGL 4.5: not implemented in this repository
 *   - Vulkan: not implemented in this repository
 *   - D3D11: not implemented in this repository
 *   - D3D12: not implemented in this repository
 *   - Metal: not implemented in this repository
 *   - NVN: not implemented in this repository
 *   - GNM: not implemented in this repository
 *
 * VCT, LPV, DDGI, path tracing and ray tracing are exposed now so the public
 * API does not need to change when future backends implement them.
 */
typedef enum HRL_EGlobalIlluminationMethod {
    HRL_GI_NONE = 0,
    HRL_GI_SSGI,
    HRL_GI_VCT,
    HRL_GI_LPV,
    HRL_GI_DDGI,
    HRL_GI_PATH_TRACING,
    HRL_GI_RAY_TRACING
} HRL_EGlobalIlluminationMethod;

typedef enum HRL_EAntialiasingMode{
	HRL_ANTIALIASING_OFF = 0,
	HRL_ANTIALIASING_2X = 2,
	HRL_ANTIALIASING_4X = 4,
	HRL_ANTIALIASING_8X = 8
}HRL_EAntialiasingMode;

typedef enum HRL_EDebugView{
	HRL_DEBUG_VIEW_NONE = 0x0060,
	HRL_DEBUG_VIEW_UNLIT,
	HRL_DEBUG_VIEW_NORMAL,
	HRL_DEBUG_VIEW_LIGHTS,
	HRL_DEBUG_VIEW_LIGHTING = HRL_DEBUG_VIEW_LIGHTS,
	HRL_DEBUG_VIEW_WIREFRAME,
	HRL_DEBUG_VIEW_LOD
}HRL_EDebugView;

typedef enum HRL_ELODMode {
	HRL_LOD_DISTANCE = 0,
	HRL_LOD_SCREEN_SIZE
} HRL_ELODMode;

typedef enum HRL_EError{
	HRL_NO_ERROR=0x0070,
	HRL_ERROR_INVALID_ID,
	HRL_INVALID_ENUM,
	HRL_INVALID_VALUE,
	HRL_INVALID_OPERATION,
	HRL_INVALID_BACKEND_OPERATION,
	HRL_SHADER_COMPILE_FAIL,
	HRL_OUT_OF_MEMORY,
	HRL_INVALID_FILE_FORMAT
}HRL_EError;
typedef enum HRL_ESeverity{
	HRL_SEVERITY_WEAK_WARNING=0x0080,
	HRL_SEVERITY_WARNING,
	HRL_SEVERITY_ERROR,
	HRL_SEVERITY_FATAL
}HRL_ESeverity;

typedef enum HRL_EFogType{
	HRL_FOG_LINEAR = 0x0090,
	HRL_FOG_EXPONENTIAL,
	HRL_FOG_EXP_SQUARED
}HRL_EFogType;

typedef enum HRL_EWidgetState{
	HRL_WIDGET_STATE_IDLE = (1 << 0),
	HRL_WIDGET_STATE_HOVERED = (1 << 1),
	HRL_WIDGET_STATE_PRESSED = (1 << 2)
}HRL_EWidgetState;

typedef enum HRL_EWidgetType{
	HRL_WIDGET_BUTTON = 0x00A0,
	HRL_WIDGET_LABEL,
	HRL_WIDGET_IMAGE,
	HRL_WIDGET_SLIDER,
	HRL_WIDGET_CHECKBOX,
	HRL_WIDGET_PROGRESSBAR
}HRL_EWidgetType;

typedef enum HRL_ESliderOrientation{
	HRL_SLIDER_HORIZONTAL = 0,
	HRL_SLIDER_VERTICAL
}HRL_ESliderOrientation;

/* Mouse callback constants. The callback receives window-relative pixel coordinates
 * through HRL_MouseMovedCallback and button transitions through HRL_MouseButtonCallback. */
#define HRL_MOUSE_BUTTON_LEFT 0
#define HRL_MOUSE_BUTTON_RIGHT 1
#define HRL_MOUSE_BUTTON_MIDDLE 2
#define HRL_MOUSE_RELEASE 0
#define HRL_MOUSE_PRESS 1


/** Default Shaders (HRL reserve theses ID) */
#define HRL_SPRITE_SHADER (UINT32_MAX)
#define HRL_MESH_2D_SHADER (UINT32_MAX - 1)
#define HRL_MESH_3D_SHADER (UINT32_MAX - 2)
#define HRL_DEBUG_SHADER (UINT32_MAX - 3)
#define HRL_DEFAULT_POST_PROCESS_SHADER (UINT32_MAX - 4)
#define HRL_SKINNED_3D_MESH_SHADER (UINT32_MAX - 5)


#ifdef __cplusplus
extern "C" {
#endif

	/* ============================================================================
	 *  INITIALIZATION & LIFECYCLE
	 * ============================================================================ */

	/**
	 * @brief Selects the graphics backend to use. Must be called before HRL_InitContext.
	 * @param _api One of the HRL_* API constants (e.g. HRL_OPENGL_45, HRL_VULKAN).
	 */
	HRL_API void HRL_Init(HRL_E_APIs _api);

	/**
	 * @brief Initializes the rendering context for the given window dimensions.
	 * @param _width  Initial framebuffer width in pixels.
	 * @param _height Initial framebuffer height in pixels.
	 * @param _loader Platform-specific function loader (eg. glfwGetProcAddress).
	 */
	HRL_API void HRL_InitContext(HRL_uint _width, HRL_uint _height, void* _loader);

	/**
	 * @brief Releases all resources and shuts down the rendering context.
	 * Must be called before the window is destroyed.
	 */
	HRL_API void HRL_Shutdown();

	/**
	 * @brief Prepares the renderer for a new frame. Call at the start of your render loop.
	 * Clears internal per-frame state and begins command recording.
	 */
	HRL_API void HRL_BeginFrame();

	/**
	 * @brief Finalizes and submits the current frame. Call at the end of your render loop.
	 * Flushes all draw commands and swaps buffers if applicable.
	 */
	HRL_API void HRL_EndFrame();

	/**
	 * @brief Notifies HRL of a window/framebuffer resize. Call from your framebuffer-size callback.
	 * The dimensions must be the drawable framebuffer size in pixels (for example,
	 * GLFW framebuffer size), not the logical window size on HiDPI displays.
	 * @param _width  New framebuffer width in pixels.
	 * @param _height New framebuffer height in pixels.
	 */
	HRL_API void HRL_WindowResizeCallback(int _width, int _height);


	/* ============================================================================
	 *  ERROR HANDLING
	 * ============================================================================ */

	/**
	 * @brief Retrieves the last recorded error, if any.
	 * @param _detail   Output pointer to a human-readable description string.
	 * @param _severity Output severity level of the error.
	 * @return The HRL_Error code. Returns HRL_NO_ERROR if no error occurred.
	 */
	HRL_API HRL_EError HRL_GetLastError(const char** _detail, HRL_ESeverity* _severity);

	/**
	 * @brief Converts an HRL_Error enum value to its string representation.
	 * @param err The error code to convert.
	 * @return A null-terminated string literal (e.g. "HRL_INVALID_ID").
	 */
	HRL_API const char* HRL_ErrorEnumToString(HRL_EError err);

	/**
	 * @brief Converts an HRL_Severity enum value to its string representation.
	 * @param sev The severity level to convert.
	 * @return A null-terminated string literal (e.g. "HRL_SEVERITY_FATAL").
	 */
	HRL_API const char* HRL_SeverityEnumToString(HRL_ESeverity sev);

	/**
	 * @brief Registers a callback invoked whenever an error is raised internally.
	 * Useful for integrating HRL errors into a custom logging or assertion system.
	 * @param _callback Function pointer with signature: void(HRL_Error, HRL_Severity, const char*).
	 */
	typedef void (*HRL_CErrorCallback)(HRL_EError code, HRL_ESeverity severity, const char* detail);
	HRL_API void HRL_RegisterErrorCallback(HRL_CErrorCallback _callback);


	/* ============================================================================
	 *  MESHES & SPRITES
	 * ============================================================================ */

	/**
	 * @brief Creates a sprite mesh in the given scene.
	 * A sprite is a textured quad that always faces the camera.
	 * @param _sceneid ID of the target scene.
	 * @return HRL_id of the new sprite, or HRL_InvalidID on failure.
	 */
	HRL_API HRL_id HRL_CreateMeshSprite(HRL_id _sceneid);

	/**
	 * @brief Sets the pivot (origin) point of a mesh or sprite.
	 * Coordinates are normalized: (0,0,0) is center, (-0.5,-0.5,0) is top-left.
	 * Affects how translation and rotation are applied to the object.
	 */
	HRL_API void HRL_SetMeshPivotPoint(HRL_id _meshid, float x, float y, float z);

	/**
	 * @brief Defines the UV region of the texture displayed on a sprite.
	 * Useful for sprite atlases. (min_u, min_v) is the top-left corner,
	 * (max_u, max_v) is the bottom-right corner, in normalized [0..1] coordinates.
	 */
	HRL_API void HRL_SetSpriteRegion(HRL_id _meshid, float min_u, float min_v, float max_u, float max_v);

	/**
	 * @brief Creates a static 3D mesh from caller-owned vertex/index arrays.
	 *
	 * The mesh data is copied by HRL and may be released by the caller as soon
	 * as the function returns. When _indexCount is zero, the vertices are drawn
	 * sequentially as triangles. Otherwise _indices must contain triangle indices
	 * and _indexCount must be a multiple of 3.
	 *
	 * No standard 3D file format importer is involved in this API.
	 *
	 * @param _sceneid      ID of the target scene.
	 * @param _vertices     Pointer to _vertexCount HRL_Vertex3D values.
	 * @param _vertexCount  Number of vertices.
	 * @param _indices      Optional pointer to _indexCount HRL_uint indices.
	 * @param _indexCount   Number of indices, or 0 for non-indexed rendering.
	 * @return HRL_id of the new mesh, or HRL_INVALID_ID on failure.
	 */
	HRL_API HRL_id HRL_CreateMesh3D(
		HRL_id _sceneid,
		const HRL_Vertex3D* _vertices, size_t _vertexCount,
		const HRL_uint* _indices, size_t _indexCount
	);

	/** Creates a skeletal mesh from CPU-side data returned by the FBX importer.
	 * HRL copies the data and the caller may free the source structure afterwards. */
	HRL_API HRL_id HRL_CreateSkeletalMesh(
		HRL_id _sceneid,
		const HRL_SkeletalMeshData* _data
	);

	/**
	 * @brief Converts an FBX mesh scene to a flat HRL_Vertex3D array.
	 *
	 * The FBX file is decoded internally by HRL and the resulting geometry is
	 * returned as non-indexed triangles. Node transforms are baked into the
	 * returned vertex positions and tangent space. The returned buffer must be
	 * released with HRL_FreeVertex3DFromFBX.
	 *
	 * HRL does not expose ufbx types through this API. The FBX implementation is
	 * an internal dependency of hrl.cpp.
	 *
	 * @param _data          Pointer to the complete FBX data in memory.
	 * @param _bufferSize    Size of the FBX data buffer in bytes.
	 * @param _vertexCount   Receives the number of returned vertices.
	 * @return Newly allocated HRL_Vertex3D array, or NULL on failure.
	 */
	HRL_API HRL_Vertex3D* HRL_GetVertex3DFromFBX(
		const char* _data,
		size_t _bufferSize,
		size_t* _vertexCount
	);

	/**
	 * @brief Frees the buffer returned by HRL_GetVertex3DFromFBX.
	 */
	HRL_API void HRL_FreeVertex3DFromFBX(HRL_Vertex3D* _vertices);

	/**
	 * @brief Converts the first skinned FBX mesh in an in-memory FBX buffer.
	 *
	 * ufbx is an internal dependency of hrl.cpp. Geometry, bones, weights and
	 * baked animation samples are copied into an HRL-owned CPU data structure.
	 * No file path is accepted by this API.
	 */
	HRL_API HRL_SkeletalMeshData* HRL_GetSkeletalMeshFromFBX(
		const char* _data, size_t _bufferSize
	);

	/** Frees all nested allocations returned by HRL_GetSkeletalMeshFromFBX. */
	HRL_API void HRL_FreeSkeletalMeshData(HRL_SkeletalMeshData* _data);

	/* ============================================================================
	 *  FBX RESOURCES & MATERIALS
	 * ============================================================================ */

	/** Opaque handle owning a decoded FBX resource scene. */
	typedef struct HRL_FBXResources HRL_FBXResources;

	/** Resource texture kind as reported by ufbx. */
	typedef enum HRL_EFBXTextureType {
		HRL_FBX_TEXTURE_FILE = 0,
		HRL_FBX_TEXTURE_LAYERED,
		HRL_FBX_TEXTURE_PROCEDURAL,
		HRL_FBX_TEXTURE_SHADER
	} HRL_EFBXTextureType;

	/** Texture slots understood by HRL's built-in 3D material shader. */
	typedef enum HRL_EFBXMaterialTextureSlot {
		HRL_FBX_MATERIAL_ALBEDO = 0,
		HRL_FBX_MATERIAL_NORMAL,
		HRL_FBX_MATERIAL_SPECULAR,
		HRL_FBX_MATERIAL_ROUGHNESS,
		HRL_FBX_MATERIAL_METALLIC,
		HRL_FBX_MATERIAL_ALPHA,
		HRL_FBX_MATERIAL_TEXTURE_SLOT_COUNT
	} HRL_EFBXMaterialTextureSlot;

	/** Description of one texture resource in an imported FBX scene.
	 * The data pointer remains valid until HRL_FreeFBXResources().
	 */
	typedef struct HRL_FBXTextureInfo {
		const char* name;
		const char* filename;
		const unsigned char* data;
		size_t size;
		HRL_EFBXTextureType type;
		int embedded;
	} HRL_FBXTextureInfo;

	/** Material information extracted from the FBX PBR/legacy material model.
	 * textureIndices contains HRL_INVALID_ID where no matching texture exists.
	 */
	typedef struct HRL_FBXMaterialInfo {
		const char* name;
		float baseColor[4];
		float roughness;
		float metallic;
		float specular;
		float opacity;
		HRL_uint textureIndices[HRL_FBX_MATERIAL_TEXTURE_SLOT_COUNT];
	} HRL_FBXMaterialInfo;

	/**
	 * @brief Loads FBX materials and textures without importing geometry.
	 * Embedded texture bytes are exposed directly through HRL_FBXTextureInfo.
	 */
	HRL_API HRL_FBXResources* HRL_LoadFBXResources(const char* _data, size_t _bufferSize);

	/** Releases a resource scene returned by HRL_LoadFBXResources(). */
	HRL_API void HRL_FreeFBXResources(HRL_FBXResources* _resources);

	HRL_API size_t HRL_GetFBXMaterialCount(const HRL_FBXResources* _resources);
	/** Returns read-only material metadata; the pointer is valid while _resources is alive. */
	HRL_API const HRL_FBXMaterialInfo* HRL_GetFBXMaterial(const HRL_FBXResources* _resources, size_t _index);
	/** Finds a material by its FBX name and returns its material index, or HRL_INVALID_ID. */
	HRL_API HRL_id HRL_FindFBXMaterial(const HRL_FBXResources* _resources, const char* _name);

	HRL_API size_t HRL_GetFBXTextureCount(const HRL_FBXResources* _resources);
	/** Returns read-only texture metadata and embedded bytes; valid while _resources is alive. */
	HRL_API const HRL_FBXTextureInfo* HRL_GetFBXTexture(const HRL_FBXResources* _resources, size_t _index);
	/** Finds a texture by FBX name or filename and returns its texture index, or HRL_INVALID_ID. */
	HRL_API HRL_id HRL_FindFBXTexture(const HRL_FBXResources* _resources, const char* _name);
	HRL_API const HRL_FBXTextureInfo* HRL_GetFBXMaterialTexture(
		const HRL_FBXResources* _resources,
		size_t _materialIndex,
		HRL_EFBXMaterialTextureSlot _slot
	);

	/** Creates an HRL GPU texture from an embedded FBX texture resource. */
	HRL_API HRL_id HRL_CreateTextureFromFBX(const HRL_FBXResources* _resources, size_t _textureIndex);

	/** Creates a material from the first FBX material. Automatically selects the built-in static or skinned 3D shader from the FBX contents. */
	HRL_API HRL_id HRL_CreateMaterialFromFBX(const char* _data, size_t _bufferSize);

	/** Creates a material from the first FBX material using a caller-selected shader. */
	HRL_API HRL_id HRL_CreateMaterialFromFBXWithShader(
		const char* _data, size_t _bufferSize, HRL_id _shaderid
	);

	/** Creates one indexed FBX material and automatically selects the built-in static or skinned 3D shader from the FBX contents. */
	HRL_API HRL_id HRL_CreateMaterialFromFBXIndexed(
		const char* _data, size_t _bufferSize, size_t _materialIndex
	);

	/** Creates one indexed FBX material using a caller-selected shader. */
	HRL_API HRL_id HRL_CreateMaterialFromFBXIndexedWithShader(
		const char* _data, size_t _bufferSize, size_t _materialIndex, HRL_id _shaderid
	);

	/**
	 * @brief Legacy generic mesh entry point.
	 *
	 * This prototype does not carry enough information to describe a bounded
	 * vertex buffer and therefore remains reserved. Use HRL_CreateMesh3D for
	 * static 3D geometry.
	 */
	HRL_API HRL_id HRL_CreateMesh(HRL_id _sceneid, HRL_EMeshType _type, const float* _vertices);

	/**
	 * @brief Reserved generic file-based mesh entry point.
	 *
	 * This entry point remains reserved. FBX conversion is intentionally exposed
	 * separately through HRL_GetVertex3DFromFBX.
	 */
	HRL_API HRL_id HRL_CreateMeshFromFile(HRL_id _sceneid, HRL_EMeshType _type, const char* _data, size_t _bufferSize);

	/**
	 * @brief Destroys a mesh and frees its associated GPU resources.
	 * @param _meshid ID of the mesh to delete.
	 */
	HRL_API void HRL_DeleteMesh(HRL_id _meshid);

	/**
	 * @brief Returns whether the given ID refers to a live mesh object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
	HRL_API int HRL_IsValidMesh(HRL_id _id);

	/**
	 * @brief Assigns a material to a mesh, controlling how it is shaded.
	 * @param _meshid ID of the target mesh.
	 * @param _matid  ID of the material to apply.
	 */
	HRL_API void HRL_SetMeshMaterial(HRL_id _meshid, HRL_id _matid);

	/**
	 * @brief Sets the world-space position of a mesh.
	 */
	HRL_API void HRL_SetMeshLocation(HRL_id _meshid, float x, float y, float z);

	/**
	 * @brief Sets the rotation of a mesh using Euler angles (in degrees).
	 * @param pitch Rotation around the X axis.
	 * @param yaw   Rotation around the Y axis.
	 * @param roll  Rotation around the Z axis.
	 */
	HRL_API void HRL_SetMeshRotation(HRL_id _meshid, float pitch, float yaw, float roll);

	/**
	 * @brief Sets the scale of a mesh along each local axis.
	 */
	HRL_API void HRL_SetMeshScale(HRL_id _meshid, float x, float y, float z);
	/** Returns the minimum world-space distance from the mesh LOD center to any viewport camera.
	 * Uses the same object-center and camera-position metric as distance-based LOD. */
	HRL_API float HRL_GetMeshCameraDistance(HRL_id _meshid);

	/** Enables or disables automatic LOD selection for a 3D mesh. */
	HRL_API void HRL_SetMeshLODAutomatic(HRL_id _meshid, int _enabled);
	/** Selects distance-based or projected screen-size LOD selection. */
	HRL_API void HRL_SetMeshLODMode(HRL_id _meshid, HRL_ELODMode _mode);
	/** Sets the number of LOD levels, including LOD 0. Range: 1..8. */
	HRL_API void HRL_SetMeshLODLevels(HRL_id _meshid, HRL_uint _levels);
	/** Sets the first transition distance in world units. */
	HRL_API void HRL_SetMeshLODDistance(HRL_id _meshid, float _baseDistance);
	/** Multiplies the distance threshold for each next LOD. */
	HRL_API void HRL_SetMeshLODScale(HRL_id _meshid, float _distanceScale);
	HRL_API void HRL_SetMeshLODMinDistance(HRL_id _meshid, float _distance);
	HRL_API void HRL_SetMeshLODMaxDistance(HRL_id _meshid, float _distance);
	/** Sets the LOD 0 screen-height threshold in [0,1]. */
	HRL_API void HRL_SetMeshLODScreenThreshold(HRL_id _meshid, float _threshold);
	/** Multiplies the screen-size threshold for each next LOD. */
	HRL_API void HRL_SetMeshLODScreenScale(HRL_id _meshid, float _scale);
	/** Adds transition hysteresis in [0,0.49] to reduce LOD popping. */
	HRL_API void HRL_SetMeshLODHysteresis(HRL_id _meshid, float _hysteresis);
	/** Forces a fixed LOD level. Pass -1 to return to automatic selection. */
	HRL_API void HRL_SetMeshLODOverride(HRL_id _meshid, int _level);
	/** Regenerates the internal LOD geometry from LOD 0. */
	HRL_API void HRL_ForceMeshLODRebuild(HRL_id _meshid);
	HRL_API HRL_uint HRL_GetMeshLODCount(HRL_id _meshid);
	HRL_API size_t HRL_GetMeshLODVertexCount(HRL_id _meshid, HRL_uint _level);
	HRL_API size_t HRL_GetMeshLODTriangleCount(HRL_id _meshid, HRL_uint _level);
	/** Returns the LOD chosen by the most recently rendered viewport. */
	HRL_API int HRL_GetMeshLODLevel(HRL_id _meshid);

	/* ============================================================================
	 *  SKELETAL MESHES
	 * ============================================================================ */

	HRL_API HRL_uint HRL_GetSkeletalBoneCount(HRL_id _meshid);
	HRL_API const HRL_SkeletalBone* HRL_GetSkeletalBone(HRL_id _meshid, HRL_uint _index);
	HRL_API HRL_uint HRL_FindSkeletalBone(HRL_id _meshid, const char* _name);

	HRL_API HRL_uint HRL_GetSkeletalAnimationCount(HRL_id _meshid);
	HRL_API const HRL_SkeletalAnimation* HRL_GetSkeletalAnimation(HRL_id _meshid, HRL_uint _index);
	HRL_API HRL_uint HRL_FindSkeletalAnimation(HRL_id _meshid, const char* _name);

	HRL_API void HRL_PlaySkeletalAnimation(HRL_id _meshid, HRL_uint _animation);
	HRL_API void HRL_StopSkeletalAnimation(HRL_id _meshid);
	HRL_API void HRL_SetSkeletalAnimationTime(HRL_id _meshid, float _time);
	HRL_API void HRL_SetSkeletalAnimationSpeed(HRL_id _meshid, float _speed);
	HRL_API void HRL_SetSkeletalAnimationLoop(HRL_id _meshid, int _loop);
	HRL_API int HRL_GetCurrentSkeletalAnimation(HRL_id _meshid);
	HRL_API float HRL_GetSkeletalAnimationTime(HRL_id _meshid);
	HRL_API int HRL_IsSkeletalAnimationPlaying(HRL_id _meshid);

	/** Advances all playing skeletal mesh animations by _deltaSeconds. */
	HRL_API void HRL_UpdateSkeletalAnimations(float _deltaSeconds);

	/**
	 * @brief Controls the rendering order of a sprite on the Z axis.
	 * Only relevant when two or more sprites share the same Z depth.
	 * Higher values are drawn on top.
	 */
	HRL_API void HRL_SetSpriteDrawOrder(HRL_id _meshid, float _draworder);


	/* ============================================================================
	 *  LIGHTS
	 * ============================================================================ */

	/**
	 * @brief Creates a light source in the given scene.
	 * @param _type One of HRL_PointLight, HRL_DirectionalLight, HRL_SpotLight.
	 * @return HRL_id of the new light, or HRL_INVALID_ID on failure.
	 */
	HRL_API HRL_id HRL_CreateLight(HRL_id _sceneid, HRL_ELightType _type);

	/**
	 * @brief Destroys a light and removes it from its scene.
	 * @param _lightid ID of the light to delete.
	 */
	HRL_API void HRL_DeleteLight(HRL_id _lightid);

	/**
	 * @brief Returns whether the given ID refers to a live light object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
	HRL_API int HRL_IsValidLight(HRL_id _id);

	/**
	 * @brief Sets the RGB color emitted by a light.
	 * Values are typically in [0..1] but may exceed 1 for HDR workflows.
	 */
	HRL_API void HRL_SetLightColor(HRL_id _lightid, float x, float y, float z);

	/**
	 * @brief Sets the intensity (brightness multiplier) of a light.
	 * @param i Intensity value. 1.0 is the default, higher values produce brighter results.
	 */
	HRL_API void HRL_SetLightIntensity(HRL_id _lightid, float i);

	/**
	 * @brief Sets the attenuation (falloff) factor of a light.
	 * Controls how quickly the light fades with distance.
	 * @param a Attenuation coefficient.
	 */
	HRL_API void HRL_SetLightAttenuation(HRL_id _lightid, float a);

	/**
	 * @brief Sets the world-space position of a light.
	 * Relevant for point lights and spot lights.
	 */
	HRL_API void HRL_SetLightLocation(HRL_id _lightid, float x, float y, float z);

	/**
	 * @brief Sets the orientation of a light using Euler angles (in degrees).
	 * Primarily relevant for directional and spot lights.
	 */
	HRL_API void HRL_SetLightRotation(HRL_id _lightid, float pitch, float yaw, float roll);

	/**
	 * @brief Enables or disables shadow casting for a light.
	 * OpenGL 3.3 supports shadows for point, directional and spot lights.
	 */
	HRL_API void HRL_SetLightCastShadows(HRL_id _lightid, int _enable);

	/**
	 * @brief Sets the shadow bias used to reduce self-shadowing artifacts.
	 */
	HRL_API void HRL_SetLightShadowBias(HRL_id _lightid, float _bias);

	/**
	 * @brief Sets the resolution of the private shadow map for a light.
	 * Supported values are positive powers of two; the OpenGL backend may clamp
	 * the requested value to the implementation limits.
	 */
	HRL_API void HRL_SetLightShadowResolution(HRL_id _lightid, int _resolution);

	/**
	 * 
	 * @param _lightid
	 * @param inner_cutoff Degrees
	 */
	HRL_API void HRL_SetSpotLightInnerCutoff(HRL_id _lightid, float inner_cutoff);

	HRL_API void HRL_SetSpotLightOuterCutoff(HRL_id _lightid, float outer_cutoff);


	/* ============================================================================
	 *  TEXTURES
	 * ============================================================================ */

	/**
	 * @brief Creates a GPU texture from a raw file buffer.
	 * Supported formats: png, jpeg, jpg, bmp, tga, gif (first frame), hdr, psd (partial).
	 * @param _data       Pointer to the file contents (opened in binary mode).
	 * @param _bufferSize Size of the buffer in bytes.
	 * @return HRL_id of the new texture, or HRL_INVALID_ID on failure.
	 */
	HRL_API HRL_id HRL_CreateTexture(const char* _data, size_t _bufferSize);

	/**
	 * @brief Destroys a texture and frees its GPU memory.
	 * @param _textureid ID of the texture to delete.
	 */
	HRL_API void HRL_DeleteTexture(HRL_id _textureid);

	/**
	 * @brief Returns whether the given ID refers to a live texture object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
	HRL_API int HRL_IsValidTexture(HRL_id _id);

	/**
	 * @brief Replaces the pixel data of an existing texture from a new file buffer.
	 * The texture ID remains valid; any material referencing it will use the updated image.
	 * @param _textureid  ID of the texture to update.
	 * @param _data       Pointer to the new file contents (opened in binary mode).
	 * @param _bufferSize Size of the buffer in bytes.
	 */
	HRL_API void HRL_ReloadTexture(HRL_id _textureid, const char* _data, size_t _bufferSize);

	/**
	 * @brief Retrieves the current dimensions of a texture in pixels.
	 * @param _width  Output width.
	 * @param _height Output height.
	 */
	HRL_API void HRL_GetTextureSize(HRL_id _textureid, int* _width, int* _height);

	/**
	 * @brief Sets the minification filter used when the texture appears smaller than its native size.
	 * @param _filter One of HRL_Filter_Nearest, HRL_Filter_Linear, HRL_Filter_Trilinear, etc.
	 */
	HRL_API void HRL_SetTextureMinFilter(HRL_id _textureid, HRL_EFilterType _filter);

	/**
	 * @brief Sets the magnification filter used when the texture appears larger than its native size.
	 * @param _filter One of HRL_Filter_Nearest, HRL_Filter_Linear, etc.
	 */
	HRL_API void HRL_SetTextureMagFilter(HRL_id _textureid, HRL_EFilterType _filter);

	/**
	 * @brief Rasterizes a UTF-8 text string into a new texture using the given font.
	 * @param _text        Null-terminated UTF-8 string to render.
	 * @param _fontid      ID of a font created with HRL_CreateFont.
	 * @param _font_size   Glyph height in pixels.
	 * @param _wrap_width  Line wrap threshold in pixels. Pass 0 to disable wrapping.
	 * @param r,g,b        Text foreground color in [0..1].
	 * @param bg_r,bg_g,bg_b,bg_a Background color. Set bg_a = 0 for a transparent background.
	 * @return HRL_id of the newly created texture, or HRL_INVALID_ID on failure.
	 */
	HRL_API HRL_id HRL_CreateTextureFromText(const char* _text, HRL_id _fontid,
		float _font_size, float _wrap_width,
		float r, float g, float b,
		float bg_r, float bg_g, float bg_b, float bg_a
	);


	/* ============================================================================
	 *  SCENES
	 * ============================================================================ */

	/**
	 * @brief Clears the entire screen to its default clear color.
	 * Useful when no scene covers the full framebuffer.
	 */
	HRL_API void HRL_ClearScreen();

	/**
	 * @brief Creates a new scene that acts as a container for meshes, lights and cameras.
	 * @param _renderOnScreen If HRL_True, the scene renders directly to the screen.
	 *                        If HRL_False, it renders into an off-screen texture buffer (default 480x480).
	 * @return HRL_id of the new scene, or HRL_INVALID_ID on failure.
	 */
	HRL_API HRL_id HRL_CreateScene(int _renderOnScreen);

	/**
	 * @brief Destroys a scene and all objects it owns.
	 * @param _sceneid ID of the scene to delete.
	 */
	HRL_API void HRL_DeleteScene(HRL_id _sceneid);

	/**
	 * @brief Returns whether the given ID refers to a live scene object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
	HRL_API int HRL_IsValidScene(HRL_id _id);

	/** Enable/disable global illumination. Disabled by default. */
	HRL_API void HRL_SetGlobalIlluminationEnabled(HRL_id _sceneid, int _enable);
	/** Select the GI method. HRL_GI_NONE disables GI for the scene. */
	HRL_API void HRL_SetGlobalIlluminationMethod(HRL_id _sceneid, HRL_EGlobalIlluminationMethod _method);
	/** Returns the scene GI enabled state. */
	HRL_API int HRL_IsGlobalIlluminationEnabled(HRL_id _sceneid);
	/** Returns the scene's selected GI method. */
	HRL_API HRL_EGlobalIlluminationMethod HRL_GetGlobalIlluminationMethod(HRL_id _sceneid);
	/** Returns whether the active backend implements this GI method. */
	HRL_API int HRL_IsGlobalIlluminationMethodSupported(HRL_EGlobalIlluminationMethod _method);
	/** Bitmask of methods supported by the active backend: (1u << method). */
	HRL_API uint32_t HRL_GetGlobalIlluminationSupportedMethods();

	/**
	 * @brief Resizes the off-screen render texture of a scene.
	 * Has no effect if the scene was created with _renderOnScreen = HRL_True.
	 */
	HRL_API void HRL_ResizeSceneTexture(HRL_id _sceneid, int _width, int _height);

	/**
	 * @brief Enables or disables the procedural sky sphere for a scene.
	 * The sky follows the camera translation, so it behaves as an infinitely
	 * distant environment while remaining a true sphere in the OpenGL backend.
	 * Disabled by default for backward compatibility.
	 */
	HRL_API void HRL_SetSkySphereEnabled(HRL_id _sceneid, int _enable);

	/**
	 * @brief Sets the top, horizon and bottom colors of the procedural sky.
	 * Values are linear RGB and may exceed 1 for HDR rendering.
	 */
	HRL_API void HRL_SetSkySphereColors(
		HRL_id _sceneid,
		float top_r, float top_g, float top_b,
		float horizon_r, float horizon_g, float horizon_b,
		float bottom_r, float bottom_g, float bottom_b
	);

	/**
	 * @brief Rotates the sky sphere using Euler angles in degrees.
	 */
	HRL_API void HRL_SetSkySphereRotation(HRL_id _sceneid, float pitch, float yaw, float roll);

	/**
	 * @brief Uses an equirectangular 2D texture as the sky-sphere image.
	 * Pass HRL_INVALID_ID to return to the procedural color sky.
	 */
	HRL_API void HRL_SetSkySphereTexture(HRL_id _sceneid, HRL_id _textureid);

	/**
	 * @brief Enables or disables equirectangular environment mapping for 3D materials.
	 * The environment texture is independent from the sky texture.
	 */
	HRL_API void HRL_SetEnvironmentMappingEnabled(HRL_id _sceneid, int _enable);

	/**
	 * @brief Sets an equirectangular texture used for environment reflections.
	 * Pass HRL_INVALID_ID to remove the environment map.
	 */
	HRL_API void HRL_SetEnvironmentMap(HRL_id _sceneid, HRL_id _textureid);

	/**
	 * @brief Enables or disables the GPU color-picking buffer for a scene.
	 * When enabled, each rendered object is assigned a unique color ID,
	 * allowing CPU-side object picking by reading pixel values.
	 * @param _enable HRL_True to enable, HRL_False to disable.
	 */
	HRL_API void HRL_EnableColorPickingBuffer(HRL_id _scene, int _enable);

	/**
	 *
	 * @param _scene
	 * @param mesh_type Reference to the type of hovered mesh, can be nullptr
	 * @param mouseX Relative mouse cursor position to the window
	 * @param mouseY
	 * @return HRL id of the hovered mesh, if none, it will return HRL_INVALID_ID
	 */
	HRL_API HRL_id HRL_GetHoveredObject(HRL_id _scene, int mouseX, int mouseY, HRL_EMeshType* mesh_type);


	/* ============================================================================
	 *  POST PROCESSING
	 * ============================================================================ */

	/**
	 * @brief Attaches a post-process pass to a viewport using a custom material.
	 * Passes are applied in creation order after the scene is rendered.
	 * @param _matid Material containing the full-screen shader to apply.
	 * @return HRL_id of the new post-process object, or HRL_INVALID_ID on failure.
	 */
	HRL_API HRL_id HRL_CreatePostProcess(HRL_id _viewport, HRL_id _matid, int priority);

	/**
	 * @brief Removes and destroys a post-process pass.
	 * @param _postid ID of the post-process to delete.
	 */
	HRL_API void HRL_DeletePostProcess(HRL_id _postid);

	/**
	 * @brief Returns whether the given ID refers to a live post-process object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
	HRL_API int HRL_IsValidPostProcess(HRL_id _id);


	/* ============================================================================
	 *  SHADERS
	 * ============================================================================ */

	/**
	 * @brief Compiles and links a shader program from GLSL vertex and fragment source.
	 * @param _vertData  Pointer to the vertex shader source buffer.
	 * @param _vertSize  Size of the vertex shader source in bytes.
	 * @param _fragData  Pointer to the fragment shader source buffer.
	 * @param _fragSize  Size of the fragment shader source in bytes.
	 * @return HRL_id of the compiled shader, or HRL_INVALID_ID on compilation failure.
	 */
	HRL_API HRL_id HRL_CreateShader(const char* _vertData, size_t _vertSize, const char* _fragData, size_t _fragSize);

	/**
	 * @brief Destroys a shader program and frees its GPU resources.
	 * @param _shaderid ID of the shader to delete.
	 */
	HRL_API void HRL_DeleteShader(HRL_id _shaderid);

	/**
	 * @brief Returns whether the given ID refers to a live shader object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
	HRL_API int HRL_IsValidShader(HRL_id _id);


	/* ============================================================================
	 *  MATERIALS
	 * ============================================================================ */

	/**
	 * @brief Creates a material instance backed by the given shader.
	 * A material stores the uniform values (textures, floats, etc.) passed to its shader.
	 * @param _shaderid ID of the shader this material uses.
	 * @return HRL_id of the new material, or HRL_INVALID_ID on failure.
	 */
	HRL_API HRL_id HRL_CreateMaterial(HRL_id _shaderid);

	/**
	 * @brief Destroys a material and its stored uniform data.
	 * @param _matid ID of the material to delete.
	 */
	HRL_API void HRL_DeleteMaterial(HRL_id _matid);

	/**
	 * @brief Returns whether the given ID refers to a live material object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
	HRL_API int HRL_IsValidMaterial(HRL_id _id);

	/**
	 * @brief Sets an integer uniform on a material.
	 */
	HRL_API void HRL_MaterialSetInt(HRL_id _matid, const char* _uniformName, int a);

	/**
	 * @brief Binds a texture to a named sampler uniform on a material.
	 */
	HRL_API void HRL_MaterialSetTexture(HRL_id _matid, const char* _uniformName, HRL_id _textureid);

	/**
	 * @brief Sets a boolean uniform on a material (internally stored as int 0 or 1).
	 */
	HRL_API void HRL_MaterialSetBool(HRL_id _matid, const char* _uniformName, int a);

	/**
	 * @brief Sets a float uniform on a material.
	 */
	HRL_API void HRL_MaterialSetFloat(HRL_id _matid, const char* _uniformName, float a);

	/**
	 * @brief Sets a vec2 uniform on a material.
	 */
	HRL_API void HRL_MaterialSetVec2(HRL_id _matid, const char* _uniformName, float x, float y);

	/**
	 * @brief Sets a vec3 uniform on a material.
	 */
	HRL_API void HRL_MaterialSetVec3(HRL_id _matid, const char* _uniformName, float x, float y, float z);

	/**
	 * @brief Sets a vec4 uniform on a material.
	 */
	HRL_API void HRL_MaterialSetVec4(HRL_id _matid, const char* _uniformName, float x, float y, float z, float w);

	/**
	 * @brief Sets the emissive color tint of a material, additively blended with the emissive texture.
	 * @param matid    ID of the target material.
	 * @param r,g,b,a  Emissive color and alpha multiplier in [0..1].
	 */
	HRL_API void HRL_MaterialSetEmissiveColor(HRL_id matid, float r, float g, float b, float a);


	/* ============================================================================
	 *  VIEWPORTS
	 * ============================================================================ */

	/**
	 * @brief Creates a viewport that renders a scene through a camera into a screen region.
	 * Useful for split-screen or picture-in-picture setups.
	 * All coordinates are normalized [0..1]: (0,0) is top-left, (1,1) is bottom-right.
	 * @param _cameraid Camera to use, or HRL_INVALID_ID to leave unassigned.
	 * @return HRL_id of the new viewport, or HRL_INVALID_ID on failure.
	 */
	HRL_API HRL_id HRL_CreateViewport(HRL_id _sceneid, HRL_id _cameraid, float x, float y, float _width, float _height);

	/**
	 * @brief Destroys a viewport.
	 * @param _viewportid ID of the viewport to delete.
	 */
	HRL_API void HRL_DeleteViewport(HRL_id _viewportid);

	/**
	 * @brief Returns whether the given ID refers to a live viewport object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
	HRL_API int HRL_IsValidViewport(HRL_id _id);

	/**
	 * @brief Reassigns the camera used by a viewport.
	 * @param _camid New camera ID, or HRL_INVALID_ID to detach.
	 */
	HRL_API void HRL_SetViewportCamera(HRL_id _viewportid, HRL_id _camid);

	/**
	 * @brief Updates the screen-space rectangle of a viewport.
	 * All values are normalized [0..1].
	 */
	HRL_API void HRL_SetViewportRect(HRL_id _viewportid, float x, float y, float _width, float _height);


	/* ============================================================================
	 *  CAMERA
	 * ============================================================================ */

	/**
	 * @brief Creates a camera in the given scene.
	 * @param _type One of HRL_Ortho or HRL_Perspective. Defaults to HRL_Ortho.
	 * @return HRL_id of the new camera, or HRL_INVALID_ID on failure.
	 */
	HRL_API HRL_id HRL_CreateCamera(HRL_id _sceneid, HRL_ECameraType _type);

	/**
	 * @brief Destroys a camera.
	 * @param _camid ID of the camera to delete.
	 */
	HRL_API void HRL_DeleteCamera(HRL_id _camid);

	/**
	 * @brief Returns whether the given ID refers to a live camera object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
	HRL_API int HRL_IsValidCamera(HRL_id _id);

	/**
	 * @brief Changes the projection type of an existing camera at runtime.
	 * @param _type HRL_Ortho or HRL_Perspective.
	 */
	HRL_API void HRL_SetCameraType(HRL_id _camid, HRL_ECameraType _type);

	/**
	 * @brief Sets the vertical extent of an orthographic camera's view volume.
	 * @param _height World-space height visible on screen.
	 */
	HRL_API void HRL_SetCameraOrthoVertical(HRL_id _camid, float _height);

	/**
	 * @brief Sets the vertical field of view for a perspective camera.
	 * @param _fov Vertical FOV in degrees.
	 */
	HRL_API void HRL_SetCameraPerspectiveFov(HRL_id _camid, float _fov);

	/**
	 * @brief Sets the near clipping plane distance.
	 * Objects closer than this value will not be rendered.
	 */
	HRL_API void HRL_SetCameraNearPlane(HRL_id _camid, float _nearPlane);

	/**
	 * @brief Sets the far clipping plane distance.
	 * Objects farther than this value will not be rendered.
	 */
	HRL_API void HRL_SetCameraFarPlane(HRL_id _camid, float _farPlane);

	/**
	 * @brief Sets the world-space position of a camera.
	 */
	HRL_API void HRL_SetCameraLocation(HRL_id _camid, float x, float y, float z);

	/**
	 * @brief Sets the orientation of a camera using Euler angles (in degrees).
	 * Axis mapping: Pitch = X, Yaw = Y, Roll = Z.
	 */
	HRL_API void HRL_SetCameraRotation(HRL_id _camid, float pitch, float yaw, float roll);


	/* ============================================================================
	 *  EFFECTS
	 * ============================================================================ *
	/* FOG */
	/**
	 * @brief Enables or disables the fog effect for a scene.
	 * @param scene  ID of the target scene.
	 * @param enable HRL_TRUE to enable, HRL_FALSE to disable.
	 */
	HRL_API void HRL_SetFogEnabled(HRL_id scene, int enable);

	/**
	 * @brief Sets the fog blending mode for a scene.
	 * @param scene ID of the target scene.
	 * @param mode  One of HRL_FOG_LINEAR, HRL_FOG_EXPONENTIAL, or HRL_FOG_EXP_SQUARED.
	 */
	HRL_API void HRL_SetFogMode(HRL_id scene, HRL_EFogType mode);

	/**
	 * @brief Sets the fog color for a scene.
	 * @param scene   ID of the target scene.
	 * @param r,g,b   Fog color in [0..1].
	 */
	HRL_API void HRL_SetFogColor(HRL_id scene, float r, float g, float b);

	/**
	 * @brief Sets the fog density for exponential fog modes.
	 * Has no effect when fog mode is HRL_FOG_LINEAR.
	 * @param scene   ID of the target scene.
	 * @param density Density coefficient. Higher values produce thicker fog.
	 */
	HRL_API void HRL_SetFogDensity(HRL_id scene, float density);

	/**
	 * @brief Sets the start and end distances for linear fog.
	 * Objects beyond _end are fully fogged; objects before _start are unaffected.
	 * @param scene  ID of the target scene.
	 * @param start  Distance at which fog begins (world units).
	 * @param end    Distance at which fog reaches full opacity (world units).
	 */
	HRL_API void HRL_SetFogLinearRange(HRL_id scene, float start, float end);

	/* VOLUMETRIC FOG */
	/**
	 * @brief Creates a localized volumetric fog volume attached to a scene.
	 * @return The new volumetric fog ID, or HRL_INVALID_ID on failure.
	 *
	 * Localized volumetric fog is an HRL object, so a scene can contain several
	 * independent volumes. The OpenGL 3.3 backend supports up to
	 * HRL_MAX_VOLUMETRIC_FOGS active volumes per scene.
	 */
	HRL_API HRL_id HRL_CreateVolumetricFog(HRL_id scene);

	/** @brief Deletes a localized volumetric fog object. */
	HRL_API void HRL_DeleteVolumetricFog(HRL_id fog);

	/** @brief Returns HRL_TRUE if the ID refers to a live volumetric fog object. */
	HRL_API int HRL_IsValidVolumetricFog(HRL_id fog);

	/** @brief Enables/disables a localized volumetric fog volume. */
	HRL_API void HRL_SetVolumetricFogEnabled(HRL_id fog, int enable);
	/** @brief Sets the center of the localized volumetric fog volume in world-space. */
	HRL_API void HRL_SetVolumetricFogPosition(HRL_id fog, float x, float y, float z);
	/** @brief Sets the radius of the localized volumetric fog volume. */
	HRL_API void HRL_SetVolumetricFogRadius(HRL_id fog, float radius);
	/** @brief Sets the density of the localized volumetric fog volume. */
	HRL_API void HRL_SetVolumetricFogDensity(HRL_id fog, float density);
	/** @brief Sets the color of the localized volumetric fog volume. */
	HRL_API void HRL_SetVolumetricFogColor(HRL_id fog, float r, float g, float b);
	/** @brief Sets the ray-march sample count for localized volumetric fog (4..64). */
	HRL_API void HRL_SetVolumetricFogSteps(HRL_id fog, HRL_uint steps);

	/* GLOBAL VOLUMETRIC FOG */
	/** @brief Enables/disables volumetric fog that fills the whole scene. */
	HRL_API void HRL_SetGlobalVolumetricFogEnabled(HRL_id scene, int enable);
	/** @brief Sets the density of the scene-wide volumetric fog. */
	HRL_API void HRL_SetGlobalVolumetricFogDensity(HRL_id scene, float density);
	/** @brief Sets the color of the scene-wide volumetric fog. */
	HRL_API void HRL_SetGlobalVolumetricFogColor(HRL_id scene, float r, float g, float b);
	/** @brief Sets the ray-march sample count for the scene-wide volumetric fog (4..64). */
	HRL_API void HRL_SetGlobalVolumetricFogSteps(HRL_id scene, HRL_uint steps);

	/** @brief Enables/disables screen-space god rays. */
	HRL_API void HRL_SetGodRaysEnabled(HRL_id scene, int enable);
	/** @brief Sets the world-space position from which god rays radiate. */
	HRL_API void HRL_SetGodRaysPosition(HRL_id scene, float x, float y, float z);
	/** @brief Sets the god-ray color. */
	HRL_API void HRL_SetGodRaysColor(HRL_id scene, float r, float g, float b);
	/** @brief Sets radial sampling density for god rays. */
	HRL_API void HRL_SetGodRaysDensity(HRL_id scene, float density);
	/** @brief Sets per-sample decay for god rays. */
	HRL_API void HRL_SetGodRaysDecay(HRL_id scene, float decay);
	/** @brief Sets the contribution weight for god rays. */
	HRL_API void HRL_SetGodRaysWeight(HRL_id scene, float weight);
	/** @brief Sets the radial sample count for god rays (8..96). */
	HRL_API void HRL_SetGodRaysSamples(HRL_id scene, HRL_uint samples);


	/* ============================================================================
	 *  MATRICES
	 * ============================================================================ */

	/**
	 * @brief Writes the current projection matrix into a caller-provided array.
	 * @param aa Pointer to a float[16] array. Matrix is column-major, contiguous.
	 */
	HRL_API void HRL_GetProjectionMatrix(float* aa);

	/**
	 * @brief Writes the current view matrix into a caller-provided array.
	 * @param aa Pointer to a float[16] array. Matrix is column-major, contiguous.
	 */
	HRL_API void HRL_GetViewMatrix(float* aa);

	/**
	 * @brief Writes the model matrix of a specific mesh into a caller-provided array.
	 * @param _meshid ID of the target mesh.
	 * @param aa      Pointer to a float[16] array. Matrix is column-major, contiguous.
	 */
	HRL_API void HRL_GetModelMatrix(HRL_id _meshid, float* aa);


	/* ============================================================================
	 *  DEBUG VIEWS
	 * ============================================================================ */

	/**
	 * @brief Overrides the scene rendering with a diagnostic visualization mode.
	 * Useful for inspecting normals, lighting, or other render passes in isolation.
	 * @param mode One of HRL_DEBUG_VIEW_NONE, HRL_DEBUG_VIEW_UNLIT, HRL_DEBUG_VIEW_NORMAL, HRL_DEBUG_VIEW_LIGHTING, HRL_DEBUG_VIEW_WIREFRAME or HRL_DEBUG_VIEW_LOD.
	 */
	HRL_API void HRL_DrawSceneAsDebugMode(HRL_id _sceneid, HRL_EDebugView mode);


	/* ============================================================================
	 *  DEBUG GEOMETRY
	 * ============================================================================ */

	/**
	 * @brief Sets the line thickness used by all debug draw calls.
	 * The exact visual result depends on the backend's line rendering support.
	 */
	HRL_API void HRL_SetDebugLineThickness(float a);

	/**
	 * @brief Draws a debug line segment for the current frame.
	 * Must be called every frame to persist the rendering.
	 * @param a_x,a_y,a_z World-space start point.
	 * @param b_x,b_y,b_z World-space end point.
	 * @param r,g,b        Line color in [0..1].
	 */
	HRL_API void HRL_DrawDebugSegment(HRL_id _sceneid,
		float a_x, float a_y, float a_z,
		float b_x, float b_y, float b_z,
		float r, float g, float b
	);

	/**
	 * @brief Draws a debug polygon (filled or outlined) for the current frame.
	 * Vertices are specified as separate X, Y and Z arrays of the same length.
	 * @param _mode         HRL_DebugHollow for outline, HRL_DebugSolid for filled.
	 * @param vertices_count Number of vertices.
	 */
	HRL_API void HRL_DrawDebugPolygon(HRL_id _sceneid, HRL_EDebugRenderingType _mode,
		const float* vertices_x, const float* vertices_y, const float* vertices_z,
		int vertices_count,
		float r, float g, float b
	);

	/**
	 * @brief Draws a debug circle for the current frame.
	 * @param _mode    HRL_DebugHollow for outline, HRL_DebugSolid for filled.
	 * @param segments Number of segments used to approximate the circle.
	 */
	HRL_API void HRL_DrawDebugCircle(HRL_id _sceneid, HRL_EDebugRenderingType _mode,
		float center_x, float center_y, float center_z,
		float radius, int segments,
		float r, float g, float b
	);

	/**
	 * @brief Draws a debug capsule (cylinder with hemispherical caps) for the current frame.
	 * @param a_x,a_y,a_z  World-space start (bottom hemisphere center).
	 * @param b_x,b_y,b_z  World-space end (top hemisphere center).
	 * @param _mode         HRL_DebugHollow or HRL_DebugSolid.
	 * @param segments      Number of segments used to approximate the capsule.
	 */
	HRL_API void HRL_DrawDebugCapsule(HRL_id _sceneid, HRL_EDebugRenderingType _mode,
		float a_x, float a_y, float a_z,
		float b_x, float b_y, float b_z,
		float radius, int segments,
		float r, float g, float b
	);

	/**
	 * @brief Draws a debug point (screen-space square) for the current frame.
	 * @param size Point size in pixels.
	 */
	HRL_API void HRL_DrawDebugPoint(HRL_id _sceneid,
		float a_x, float a_y, float a_z,
		float size,
		float r, float g, float b
	);


	/* ============================================================================
	 *  UTILITY
	 * ============================================================================ */

	/**
	 * @brief Captures the rendered output of a scene and saves it as a PNG file.
	 * The request is queued and consumed at the end of the current frame.
	 * On OpenGL 3.3 the capture includes the final on-screen post-process result.
	 * @param _target_path Absolute path where the PNG image will be written.
	 */
	HRL_API void HRL_TakeScreenshot(HRL_id _sceneid, const char* _target_path);

	/**
	 * @brief Enables or changes the global multisample anti-aliasing mode.
	 * @param _mode One of HRL_ANTIALIASING_OFF, HRL_ANTIALIASING_2X,
	 *              HRL_ANTIALIASING_4X or HRL_ANTIALIASING_8X.
	 */
	HRL_API void HRL_SetAntialiasingMode(HRL_uint _mode);


	/* ============================================================================
	 *  FONTS
	 * ============================================================================ */

	/**
	 * @brief Loads a TrueType font from a memory buffer for use with HRL_CreateTextureFromText.
	 * @param data       Pointer to the raw .ttf file contents.
	 * @param _data_size Size of the buffer in bytes.
	 * @return HRL_id of the new font, or HRL_INVALID_ID on failure.
	 */
	HRL_API HRL_id HRL_CreateFont(const char* _data, size_t _data_size);

	/**
	 * @brief Destroys a font and frees its associated resources.
	 * @param _fontid ID of the font to delete.
	 */
	HRL_API void HRL_DeleteFont(HRL_id _fontid);

	/**
	 * @brief Returns whether the given ID refers to a live font object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
	HRL_API int HRL_IsValidFont(HRL_id _id);


	/* ============================================================================
	 *  UI
	 * ============================================================================ */

	/** Mouse input is window-relative in pixels. Call these from the application
	 * input callbacks before HRL_EndFrame(). */
	HRL_API void HRL_MouseMovedCallback(float x, float y);
	HRL_API void HRL_MouseButtonCallback(int button, int pressed);

	/** Widget positions are normalized to their owning viewport. Widget sizes use the
	 * existing normalized API as the initial size, then keep the resulting pixel size
	 * across window resizes so aspect-ratio changes do not stretch controls. */
	HRL_API HRL_id HRL_CreateWidget(HRL_id viewport, HRL_EWidgetType type);
	HRL_API void HRL_DeleteWidget(HRL_id widget);
	HRL_API int HRL_IsValidWidget(HRL_id widget);

	HRL_API void HRL_SetWidgetPosition(HRL_id widget, float x, float y);
	HRL_API void HRL_SetWidgetSize(HRL_id widget, float width, float height);
	HRL_API void HRL_SetWidgetAlpha(HRL_id widget, float a);
	HRL_API void HRL_SetWidgetAnchor(HRL_id widget, float ax, float ay);
	HRL_API void HRL_SetWidgetVisible(HRL_id widget, int visible);
	HRL_API void HRL_SetWidgetEnabled(HRL_id widget, int enabled);
	HRL_API void HRL_SetWidgetZIndex(HRL_id widget, int z_index);
	HRL_API int HRL_IsWidgetHovered(HRL_id widget);

	/* BUTTON */
	HRL_API void HRL_SetButtonClickable(HRL_id widget, int clickable);
	HRL_API void HRL_SetButtonText(HRL_id widget, const char* text);
	HRL_API void HRL_SetButtonTextSize(HRL_id widget, float size);
	HRL_API void HRL_SetButtonTextTintColor(HRL_id widget, HRL_EWidgetState state, float r, float g, float b, float a);
	HRL_API void HRL_SetButtonTextFont(HRL_id widget, HRL_id font);
	HRL_API void HRL_SetButtonBackgroundTexture(HRL_id widget, HRL_EWidgetState state, HRL_id texture);
	HRL_API void HRL_SetButtonBackgroundTintColor(HRL_id widget, HRL_EWidgetState state, float r, float g, float b, float a);
	typedef void(*HRL_CButtonPressed)(HRL_id button, int clicked, int released, void* user_data);
	HRL_API void HRL_SetButtonPressedCallback(HRL_id widget, HRL_CButtonPressed callback, void* user_data);

	/* LABEL */
	HRL_API void HRL_SetLabelText(HRL_id widget, const char* text);
	HRL_API void HRL_SetLabelTextSize(HRL_id widget, float size);
	HRL_API void HRL_SetLabelFont(HRL_id widget, HRL_id font);
	HRL_API void HRL_SetLabelTintColor(HRL_id widget, float r, float g, float b, float a);

	/* IMAGE */
	HRL_API void HRL_SetImageTexture(HRL_id widget, HRL_id texture);
	HRL_API void HRL_SetImageTintColor(HRL_id widget, float r, float g, float b, float a);

	/* SLIDER */
	typedef void(*HRL_CSliderChanged)(HRL_id slider, float value, void* user_data);
	HRL_API void HRL_SetSliderRange(HRL_id widget, float minimum, float maximum);
	HRL_API void HRL_SetSliderValue(HRL_id widget, float value);
	HRL_API float HRL_GetSliderValue(HRL_id widget);
	HRL_API void HRL_SetSliderOrientation(HRL_id widget, HRL_ESliderOrientation orientation);
	HRL_API void HRL_SetSliderClickable(HRL_id widget, int clickable);
	HRL_API void HRL_SetSliderBackgroundColor(HRL_id widget, float r, float g, float b, float a);
	HRL_API void HRL_SetSliderFillColor(HRL_id widget, float r, float g, float b, float a);
	HRL_API void HRL_SetSliderHandleColor(HRL_id widget, float r, float g, float b, float a);
	HRL_API void HRL_SetSliderChangedCallback(HRL_id widget, HRL_CSliderChanged callback, void* user_data);

	/* CHECKBOX */
	typedef void(*HRL_CCheckboxChanged)(HRL_id checkbox, int checked, void* user_data);
	HRL_API void HRL_SetCheckboxChecked(HRL_id widget, int checked);
	HRL_API int HRL_IsCheckboxChecked(HRL_id widget);
	HRL_API void HRL_SetCheckboxClickable(HRL_id widget, int clickable);
	HRL_API void HRL_SetCheckboxBackgroundColor(HRL_id widget, float r, float g, float b, float a);
	HRL_API void HRL_SetCheckboxCheckedColor(HRL_id widget, float r, float g, float b, float a);
	HRL_API void HRL_SetCheckboxChangedCallback(HRL_id widget, HRL_CCheckboxChanged callback, void* user_data);

	/* PROGRESS BAR */
	HRL_API void HRL_SetProgressBarValue(HRL_id widget, float value);
	HRL_API float HRL_GetProgressBarValue(HRL_id widget);
	HRL_API void HRL_SetProgressBarBackgroundColor(HRL_id widget, float r, float g, float b, float a);
	HRL_API void HRL_SetProgressBarFillColor(HRL_id widget, float r, float g, float b, float a);


#ifdef __cplusplus
} /* extern "C" */
#endif


/* ============================================================================
 *  UTILITY MACROS
 * ============================================================================ */

/**
 * @brief Polls and prints the last HRL error to stdout, if any.
 * Includes the error code, severity, detail message, and source location.
 * Intended for debug builds; wrap in #ifdef _DEBUG if needed.
 */
#define HRL_CheckErrors() do { \
	const char* e = nullptr; \
	HRL_ESeverity sev=HRL_SEVERITY_WEAK_WARNING; \
	HRL_EError err=HRL_GetLastError(&e, &sev); \
		if (err != HRL_NO_ERROR) \
			printf("[HRL] %d:%d | %s | %s:%d\n", err, sev, e, __FILE__, __LINE__); \
	} while(0)


#endif /* HRL_IMPL */