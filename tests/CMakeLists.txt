# HRL tests. Built with -DHRL_BUILD_TESTS=ON, run with `ctest`.
if(HRL_ENABLE_VULKAN)
    # Off-screen: needs a Vulkan 1.3 driver (a software one such as Mesa
    # lavapipe is enough) but no window system.
    add_executable(hrl_test_vulkan vulkan_smoke.cpp)
    target_link_libraries(hrl_test_vulkan PRIVATE hrldll)
    add_test(NAME vulkan_smoke COMMAND hrl_test_vulkan WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME vulkan_smoke_no_raytracing COMMAND hrl_test_vulkan WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    # Validation layers turn every Vulkan misuse into a test failure when installed.
    set_tests_properties(vulkan_smoke PROPERTIES ENVIRONMENT "HRL_VULKAN_VALIDATION=1")
    set_tests_properties(vulkan_smoke_no_raytracing PROPERTIES ENVIRONMENT "HRL_VULKAN_VALIDATION=1;HRL_VULKAN_NO_RAYTRACING=1")

    # Landscapes, foliage, grass, GPU culling.
    add_executable(hrl_test_vulkan_nature vulkan_nature.cpp)
    target_link_libraries(hrl_test_vulkan_nature PRIVATE hrldll)
    add_test(NAME vulkan_nature COMMAND hrl_test_vulkan_nature WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME vulkan_nature_no_raytracing COMMAND hrl_test_vulkan_nature WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(vulkan_nature PROPERTIES ENVIRONMENT "HRL_VULKAN_VALIDATION=1")
    set_tests_properties(vulkan_nature_no_raytracing PROPERTIES ENVIRONMENT "HRL_VULKAN_VALIDATION=1;HRL_VULKAN_NO_RAYTRACING=1")

    # CPU cost of preparing a large scene (prints timings, see docs/ARCHITECTURE.md).
    add_executable(hrl_bench_scene bench_scene.cpp)
    target_link_libraries(hrl_bench_scene PRIVATE hrldll)

    # The Vulkan backend must stay independent from the frontend state.
    add_test(NAME backend_isolation
        COMMAND ${CMAKE_COMMAND} -DBACKEND_DIR=${CMAKE_SOURCE_DIR}/src/backend/vulkan
                -P ${CMAKE_CURRENT_SOURCE_DIR}/check_backend_isolation.cmake)
endif()

# OpenGL regression test: needs GLFW (available when the examples are built)
# and a display. Exit code 77 = skipped (no display / no OpenGL 3.3).
if(TARGET glfw)
    add_executable(hrl_test_opengl opengl_smoke.cpp)
    target_link_libraries(hrl_test_opengl PRIVATE hrldll glfw)
    add_test(NAME opengl_smoke COMMAND hrl_test_opengl WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(opengl_smoke PROPERTIES SKIP_RETURN_CODE 77)
endif()
