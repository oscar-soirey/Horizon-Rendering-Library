# HRL scene editor example: interface made with Raven, 3D view rendered by the
# Vulkan backend of HRL. Built with the other examples (HRL_BUILD_EXAMPLES).
#
# Raven (https://github.com/oscar-soirey/Raven) is taken from HRL_RAVEN_DIR
# when it points to a checkout, otherwise it is downloaded once at configure
# time. It is built as a static library; only the editor links it.
set(HRL_RAVEN_DIR "" CACHE PATH "Local checkout of Raven used by the editor example (empty: download it)")
set(HRL_RAVEN_GIT_TAG "8c6770aabdc7b0a6ff97a1f265808303a86bca49" CACHE STRING "Raven commit downloaded for the editor example")

set(RAVEN_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(RAVEN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RAVEN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RAVEN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(RAVEN_BUILD_WEB OFF CACHE BOOL "" FORCE)
if(HRL_RAVEN_DIR)
    add_subdirectory(${HRL_RAVEN_DIR} ${CMAKE_BINARY_DIR}/raven EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(raven GIT_REPOSITORY https://github.com/oscar-soirey/Raven.git GIT_TAG ${HRL_RAVEN_GIT_TAG})
    FetchContent_MakeAvailable(raven)
endif()

add_executable(hrl_editor main.cpp native_surface.cpp native_surface.h)
# Vulkan::Vulkan: the example creates the VkSurfaceKHR of the viewport itself.
target_link_libraries(hrl_editor PRIVATE hrldll raven::raven Vulkan::Vulkan)
if(UNIX)
    find_package(X11 REQUIRED)
    target_include_directories(hrl_editor PRIVATE ${X11_INCLUDE_DIR})
endif()
