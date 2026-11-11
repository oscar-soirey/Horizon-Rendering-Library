cmake_minimum_required(VERSION 3.20)
project(hrl LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Built as the main project: the library, the examples and the tests go to one
# folder, so the executables find the HRL shared library without any copy.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# Vulkan is the primary backend. OpenGL 3.3 is deprecated: it is still built
# for existing projects but receives no new feature.
option(HRL_ENABLE_VULKAN "Compile the native Vulkan 1.3 backend (HRL_VULKAN)" ON)
option(HRL_ENABLE_GLES3 "Compile the OpenGL ES 3.0 backend (HRL_OPENGL_ES_30)" OFF)
option(HRL_BUILD_STATIC "Also build the static library target `hrl`" OFF)
option(HRL_BUILD_EXAMPLES "Build the examples (GLFW and Raven are fetched when not available)" OFF)
option(HRL_BUILD_TESTS "Build the tests (run them with ctest)" OFF)
set(HRL_VULKAN_SDK "" CACHE PATH "Vulkan SDK root, when the VULKAN_SDK environment variable is not set")

# Former option name, kept so existing build scripts still work.
if(DEFINED COMPILE_VULKAN)
    set(HRL_ENABLE_VULKAN ${COMPILE_VULKAN} CACHE BOOL "" FORCE)
endif()

find_package(Python REQUIRED COMPONENTS Interpreter)

set(HRL_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${HRL_GENERATED_DIR})

# ---------------------------------------------------------------------------
# Embedded resources of the OpenGL backends (GLSL sources, default textures)
# ---------------------------------------------------------------------------
function(embed_binary INPUT_FILE OUTPUT_NAME)
    set(OUTPUT_CPP ${HRL_GENERATED_DIR}/${OUTPUT_NAME}.cpp)
    add_custom_command(
        OUTPUT ${OUTPUT_CPP}
        COMMAND ${Python_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/embed.py
                ${INPUT_FILE} ${OUTPUT_NAME} > ${OUTPUT_CPP}
        DEPENDS ${INPUT_FILE} ${CMAKE_SOURCE_DIR}/tools/embed.py
        COMMENT "Embedding ${INPUT_FILE}"
    )
    set(EMBEDDED_SOURCES ${EMBEDDED_SOURCES} ${OUTPUT_CPP} PARENT_SCOPE)
    set(EMBEDDED_RESOURCES ${EMBEDDED_RESOURCES} ${OUTPUT_NAME} PARENT_SCOPE)
endfunction()

file(GLOB_RECURSE RESSOURCES_FILES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/shaders/opengl/*.glsl
    ${CMAKE_SOURCE_DIR}/src/ressources/textures/*.png
)
foreach(RES_FILE ${RESSOURCES_FILES})
    get_filename_component(FILE_NAME ${RES_FILE} NAME)
    string(REPLACE "." "_" BASE_NAME ${FILE_NAME})
    embed_binary(${RES_FILE} res_${BASE_NAME})
endforeach()

# src/ressources/ressources.h declares the embedded symbols. It is only
# rewritten when its content changes, so a configure does not rebuild HRL.
set(RESSOURCES_H_CONTENT "#ifndef GENERATED_RESSOURCES_H\n#define GENERATED_RESSOURCES_H\n\n#include <cstddef>\n\n")
foreach(RES ${EMBEDDED_RESOURCES})
    string(APPEND RESSOURCES_H_CONTENT "//${RES}\nextern unsigned char ${RES}[];\nextern size_t ${RES}_len;\n\n")
endforeach()
string(APPEND RESSOURCES_H_CONTENT "#endif\n")
set(RESSOURCES_H ${CMAKE_SOURCE_DIR}/src/ressources/ressources.h)
set(RESSOURCES_H_OLD "")
if(EXISTS ${RESSOURCES_H})
    file(READ ${RESSOURCES_H} RESSOURCES_H_OLD)
endif()
if(NOT RESSOURCES_H_OLD STREQUAL RESSOURCES_H_CONTENT)
    file(WRITE ${RESSOURCES_H} "${RESSOURCES_H_CONTENT}")
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(HRL_SOURCES
    third_party/glad/glad.c
    third_party/ufbx/ufbx.c
    ${EMBEDDED_SOURCES}

    # Frontend: implementation of the public API, one file per domain
    src/frontend/context.cpp
    src/frontend/scene.cpp
    src/frontend/camera.cpp
    src/frontend/mesh.cpp
    src/frontend/skeletal.cpp
    src/frontend/landscape.cpp
    src/frontend/light.cpp
    src/frontend/material.cpp
    src/frontend/texture.cpp
    src/frontend/fbx.cpp
    src/frontend/effects.cpp
    src/frontend/vfx.cpp
    src/frontend/voxel.cpp
    src/frontend/gizmo.cpp
    src/frontend/widget.cpp
    src/frontend/debug.cpp

    # Core: CPU systems shared by the frontend and the backends
    src/core/utils_functions.cpp
    src/core/widgets.cpp
    src/core/render_common.cpp
    src/core/voxel_render.cpp
    src/core/gi_scene.cpp

    # OpenGL 3.3 backend (deprecated)
    src/backend/opengl33/gl33_backend.cpp
    src/backend/opengl33/gl33_renderer.cpp
    src/backend/opengl33/gl33_shader.cpp
    src/backend/opengl33/gl33_texture.cpp
    src/backend/opengl33/gi.cpp
)

if(HRL_ENABLE_GLES3)
    list(APPEND HRL_SOURCES
        src/backend/gles3/gles3_backend.cpp
        src/backend/gles3/gles3_renderer.cpp
        src/backend/gles3/gles3_shader.cpp
        src/backend/gles3/gles3_texture.cpp
    )
endif()

# ---------------------------------------------------------------------------
# Vulkan backend: GLSL -> SPIR-V at build time, embedded in the library.
# Nothing is compiled at run time and no SDK is needed on the user's machine.
# ---------------------------------------------------------------------------
if(HRL_ENABLE_VULKAN)
    if(HRL_VULKAN_SDK)
        set(ENV{VULKAN_SDK} "${HRL_VULKAN_SDK}")
    endif()
    find_package(Vulkan)
    if(NOT Vulkan_FOUND)
        message(FATAL_ERROR
            "HRL: the Vulkan SDK was not found. Install it (https://vulkan.lunarg.com), "
            "or pass -DHRL_VULKAN_SDK=<path>, or disable the backend with -DHRL_ENABLE_VULKAN=OFF.")
    endif()
    find_program(HRL_GLSLANG NAMES glslangValidator glslang
        HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" "${HRL_VULKAN_SDK}/Bin" "${HRL_VULKAN_SDK}/bin")
    if(NOT HRL_GLSLANG)
        message(FATAL_ERROR "HRL: glslangValidator (part of the Vulkan SDK) was not found.")
    endif()

    set(HRL_VK_SHADER_DIR ${CMAKE_SOURCE_DIR}/src/shaders/vulkan)
    file(GLOB HRL_VK_SHADER_INCLUDES CONFIGURE_DEPENDS ${HRL_VK_SHADER_DIR}/*.glsl)
    set(HRL_VK_SPIRV "")
    set(HRL_VK_EMBED_ARGS "")
    # hrl_vk_shader(<embedded name> <source file> [defines...])
    function(hrl_vk_shader NAME FILE)
        set(SPV ${HRL_GENERATED_DIR}/${NAME}.spv)
        set(DEFINES "")
        foreach(DEF ${ARGN})
            list(APPEND DEFINES -D${DEF})
        endforeach()
        add_custom_command(
            OUTPUT ${SPV}
            COMMAND ${HRL_GLSLANG} -V --quiet --target-env vulkan1.3 -I${HRL_VK_SHADER_DIR}
                    ${DEFINES} ${HRL_VK_SHADER_DIR}/${FILE} -o ${SPV}
            DEPENDS ${HRL_VK_SHADER_DIR}/${FILE} ${HRL_VK_SHADER_INCLUDES}
            COMMENT "SPIR-V ${NAME}"
            VERBATIM
        )
        set(HRL_VK_SPIRV ${HRL_VK_SPIRV} ${SPV} PARENT_SCOPE)
        set(HRL_VK_EMBED_ARGS ${HRL_VK_EMBED_ARGS} ${NAME}=${SPV} PARENT_SCOPE)
    endfunction()

    hrl_vk_shader(mesh_vert        mesh.vert)
    hrl_vk_shader(mesh_frag        mesh.frag)
    hrl_vk_shader(mesh_at_frag     mesh.frag HRL_ALPHA_TEST)
    hrl_vk_shader(mesh_rq_frag     mesh.frag HRL_RAY_QUERY)
    hrl_vk_shader(mesh_at_rq_frag  mesh.frag HRL_ALPHA_TEST HRL_RAY_QUERY)
    hrl_vk_shader(shadow_vert      shadow.vert)
    hrl_vk_shader(shadow_at_vert   shadow.vert HRL_ALPHA_TEST)
    hrl_vk_shader(shadow_frag      shadow.frag)
    hrl_vk_shader(fullscreen_vert  fullscreen.vert)
    hrl_vk_shader(sky_frag         sky.frag)
    hrl_vk_shader(tonemap_frag     tonemap.frag)
    hrl_vk_shader(bloom_frag       bloom.frag)
    hrl_vk_shader(skin_comp        skin.comp)
    hrl_vk_shader(debug_vert       debug.vert)
    hrl_vk_shader(debug_frag       debug.frag)
    hrl_vk_shader(env_cube_comp    env_cube.comp)
    hrl_vk_shader(env_filter_comp  env_filter.comp)
    hrl_vk_shader(brdf_lut_comp    brdf_lut.comp)

    add_custom_command(
        OUTPUT ${HRL_GENERATED_DIR}/hrl_vk_shaders.cpp ${HRL_GENERATED_DIR}/hrl_vk_shaders.h
        COMMAND ${Python_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/embed_spirv.py
                ${HRL_GENERATED_DIR}/hrl_vk_shaders.cpp ${HRL_GENERATED_DIR}/hrl_vk_shaders.h ${HRL_VK_EMBED_ARGS}
        DEPENDS ${HRL_VK_SPIRV} ${CMAKE_SOURCE_DIR}/tools/embed_spirv.py
        COMMENT "Embedding Vulkan shaders"
        VERBATIM
    )

    list(APPEND HRL_SOURCES
        ${HRL_GENERATED_DIR}/hrl_vk_shaders.cpp
        src/backend/vulkan/vk_mem_alloc.cpp
        src/backend/vulkan/vk_device.cpp
        src/backend/vulkan/vk_frame_graph.cpp
        src/backend/vulkan/vk_jobs.cpp
        src/backend/vulkan/vk_pipelines.cpp
        src/backend/vulkan/vk_resources.cpp
        src/backend/vulkan/vk_environment.cpp
        src/backend/vulkan/vk_scene_data.cpp
        src/backend/vulkan/vk_renderer.cpp
        src/backend/vulkan/vulkan_backend.cpp
    )
endif()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
find_package(OpenGL)
find_package(Threads REQUIRED)

function(hrl_configure_target TARGET)
    target_include_directories(${TARGET}
        PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        PRIVATE ${CMAKE_SOURCE_DIR}/third_party ${HRL_GENERATED_DIR})
    target_link_libraries(${TARGET} PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    # M_PI with every compiler, no min/max macros from windows.h.
    target_compile_definitions(${TARGET} PRIVATE _USE_MATH_DEFINES NOMINMAX _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE /utf-8 /bigobj)
    endif()
    if(OpenGL_FOUND)
        target_link_libraries(${TARGET} PRIVATE OpenGL::GL)
    endif()
    if(HRL_ENABLE_GLES3)
        target_compile_definitions(${TARGET} PRIVATE HRL_ENABLE_GLES3)
    endif()
    if(HRL_ENABLE_VULKAN)
        target_compile_definitions(${TARGET} PRIVATE HRL_ENABLE_VULKAN)
        target_link_libraries(${TARGET} PRIVATE Vulkan::Vulkan)
    endif()
endfunction()

add_library(hrldll SHARED ${HRL_SOURCES})
target_compile_definitions(hrldll PRIVATE HRL_BUILD_DLL)
hrl_configure_target(hrldll)
add_library(HRL::hrl ALIAS hrldll)

if(HRL_BUILD_STATIC)
    add_library(hrl STATIC ${HRL_SOURCES})
    target_compile_definitions(hrl PUBLIC HRL_NO_DLL)
    hrl_configure_target(hrl)
endif()

if(HRL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(HRL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
