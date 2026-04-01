cmake_minimum_required(VERSION 3.31)
project(hrl)

set(CMAKE_CXX_STANDARD 20)

#debug messages
set(CMAKE_MESSAGE_LOG_LEVEL DEBUG)

include_directories(src)
include_directories(third_party)

find_package(Python REQUIRED)

#creer le fichier generated dans le fichier cmake
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)

#function to generate a .cpp file with binary definition of a file
function(embed_binary INPUT_FILE OUTPUT_NAME)
    set(OUTPUT_CPP ${CMAKE_BINARY_DIR}/generated/${OUTPUT_NAME}.cpp)

    add_custom_command(
        OUTPUT ${OUTPUT_CPP}
        COMMAND ${Python_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/embed.py
                ${INPUT_FILE} ${OUTPUT_NAME} > ${OUTPUT_CPP}
        DEPENDS ${INPUT_FILE}
        COMMENT "Embeding ${INPUT_FILE}"
    )

    #on marque ces fichier .cpp comme generés
    set_source_files_properties(${OUTPUT_CPP}
            PROPERTIES GENERATED TRUE
    )

    # cpp générés
    set(EMBEDDED_SOURCES ${EMBEDDED_SOURCES} ${OUTPUT_CPP} PARENT_SCOPE)

    # noms des ressources (IMPORTANT)
    set(EMBEDDED_RESOURCES ${EMBEDDED_RESOURCES} ${OUTPUT_NAME} PARENT_SCOPE)
endfunction()


#trouver tous les fichiers ressources
file(GLOB_RECURSE RESSOURCES_FILES
        CONFIGURE_DEPENDS
        #shaders
        ${CMAKE_SOURCE_DIR}/src/shaders/*.glsl
        ${CMAKE_SOURCE_DIR}/src/shaders/*.hlsl
        ${CMAKE_SOURCE_DIR}/src/shaders/*.metal

        #textures
        ${CMAKE_SOURCE_DIR}/src/ressources/textures/*.png
)

foreach(RES_FILE ${RESSOURCES_FILES})
    #nom du fichier sans chemin
    get_filename_component(FILE_NAME ${RES_FILE} NAME) #ex: sprite.vert.glsl

    #sprite.vert -> sprite_vert exemple
    string(REPLACE "." "_" BASE_NAME ${FILE_NAME})

    #prefix res_
    set(RESOURCE_NAME res_${BASE_NAME})

    embed_binary(${RES_FILE} ${RESOURCE_NAME})
endforeach()


#generer automatiquement le fichier ressources.h
function(generate_ressources_file_header)
    set(RESSOURCES_H ${CMAKE_SOURCE_DIR}/src/ressources/ressources.h)
    file(WRITE ${RESSOURCES_H}
            "#ifndef GENERATED_RESSOURCES_H\n#define GENERATED_RESSOURCES_H\n\n#include <cstddef>\n\n"
    )
    foreach(RES ${EMBEDDED_RESOURCES})
        file(APPEND ${RESSOURCES_H}
                "//${RES}\n"
                "extern unsigned char ${RES}[];\n"
                "extern size_t ${RES}_len;\n\n"
        )
    endforeach()
    file(APPEND ${RESSOURCES_H}
            "#endif\n"
    )
    set_source_files_properties(${RESSOURCES_H}
            PROPERTIES GENERATED TRUE
    )
endfunction()

generate_ressources_file_header()

add_library(hrl STATIC
        #third-party
        third_party/glad/glad.c

        #fichiers ressources générés
        ${EMBEDDED_SOURCES}

        src/hrl.cpp

        #core
        src/core/utils_functions.cpp

        #opengl
        src/backend/opengl33/gl33_backend.cpp
        src/backend/opengl33/gl33_renderer.cpp
        src/backend/opengl33/gl33_shader.cpp
        src/backend/opengl33/gl33_texture.cpp
)

add_library(hrldll SHARED
        #third-party
        third_party/glad/glad.c

        #fichiers ressources générés
        ${EMBEDDED_SOURCES}

        src/hrl.cpp

        #core
        src/core/utils_functions.cpp

        #opengl
        src/backend/opengl33/gl33_backend.cpp
        src/backend/opengl33/gl33_renderer.cpp
        src/backend/opengl33/gl33_shader.cpp
        src/backend/opengl33/gl33_texture.cpp
)

#compile dll declspec export
target_compile_definitions(hrl PRIVATE HRL_NO_DLL)
target_compile_definitions(hrldll PRIVATE HRL_BUILD_DLL)


#link OpenGL
find_package(OpenGL REQUIRED)
target_link_libraries(hrl PRIVATE OpenGL::GL)
target_link_libraries(hrldll PRIVATE OpenGL::GL)