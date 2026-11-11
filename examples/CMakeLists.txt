# HRL examples, built with the library: cmake -DHRL_BUILD_EXAMPLES=ON
#
# The examples link the `hrldll` target and include the headers of src/
# directly: nothing is copied. Executables and the HRL library are written to
# the same folder (<build>/bin, or <build>/bin/<Config> with Visual Studio).
#
# GLFW is used for the windows: taken from the system when installed,
# otherwise downloaded and built once (needs network access at configure time).
option(HRL_EXAMPLES_FETCH_GLFW "Download GLFW when it is not installed" ON)

find_package(glfw3 3.3 QUIET)
if(NOT glfw3_FOUND AND HRL_EXAMPLES_FETCH_GLFW)
    include(FetchContent)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    # Linux: build the Wayland support only when its development packages are
    # installed (X11 windows also work under Wayland through XWayland).
    if(UNIX AND NOT APPLE AND NOT DEFINED GLFW_BUILD_WAYLAND)
        find_package(PkgConfig QUIET)
        set(HRL_GLFW_WAYLAND OFF)
        if(PkgConfig_FOUND)
            pkg_check_modules(HRL_WAYLAND_DEPS QUIET wayland-client wayland-cursor wayland-egl xkbcommon)
            if(HRL_WAYLAND_DEPS_FOUND)
                set(HRL_GLFW_WAYLAND ON)
            endif()
        endif()
        set(GLFW_BUILD_WAYLAND ${HRL_GLFW_WAYLAND} CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(glfw)
    set(glfw3_FOUND TRUE)
endif()

# ---- Vulkan: physically based 3D scene ---------------------------------------
if(HRL_ENABLE_VULKAN)
    add_executable(hrl_vulkan_pbr vulkan_pbr/main.cpp)
    target_link_libraries(hrl_vulkan_pbr PRIVATE hrldll)
    if(glfw3_FOUND)
        target_link_libraries(hrl_vulkan_pbr PRIVATE glfw)
        target_compile_definitions(hrl_vulkan_pbr PRIVATE HRL_EXAMPLE_HAS_GLFW)
    endif()
endif()

# ---- Vulkan: scene editor with a Raven interface -------------------------------
if(HRL_ENABLE_VULKAN AND NOT APPLE)
    add_subdirectory(editor)
endif()

# ---- OpenGL 3.3 (deprecated backend) -----------------------------------------
# Resources (textures, FBX, fonts) are read from examples/assets, which is not
# versioned: see examples/README.md.
if(NOT glfw3_FOUND)
    message(STATUS "HRL: GLFW not available, the OpenGL examples are skipped")
    return()
endif()

add_library(hrl_example_common STATIC opengl/src/example.cpp)
target_include_directories(hrl_example_common PUBLIC opengl/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(hrl_example_common PUBLIC hrldll glfw)
target_compile_definitions(hrl_example_common PRIVATE HRL_EXAMPLE_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")

# showcase and minimal define GLFW_INCLUDE_VULKAN (their Vulkan surface
# callback uses VkInstance / VkSurfaceKHR): they need the Vulkan SDK headers.
if(NOT TARGET Vulkan::Headers)
    find_package(Vulkan QUIET)
endif()
set(HRL_GL_EXAMPLES vfx voxel widgets)
if(TARGET Vulkan::Headers)
    list(APPEND HRL_GL_EXAMPLES showcase minimal)
else()
    message(STATUS "HRL: Vulkan SDK headers not found, hrl_gl_showcase and hrl_gl_minimal are skipped")
endif()

foreach(EXAMPLE ${HRL_GL_EXAMPLES})
    add_executable(hrl_gl_${EXAMPLE} opengl/${EXAMPLE}.cpp)
    target_link_libraries(hrl_gl_${EXAMPLE} PRIVATE hrl_example_common)
    # Visual Studio: start the debugger in the asset folder too.
    set_target_properties(hrl_gl_${EXAMPLE} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/assets)
endforeach()
foreach(EXAMPLE showcase minimal)
    if(TARGET hrl_gl_${EXAMPLE})
        target_link_libraries(hrl_gl_${EXAMPLE} PRIVATE Vulkan::Headers)
    endif()
endforeach()
