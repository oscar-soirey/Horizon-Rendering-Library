# HRL tests. Built with -DHRL_BUILD_TESTS=ON, run with `ctest`.
# The Vulkan tests render off-screen: they need a Vulkan 1.3 driver (a software
# one such as Mesa lavapipe is enough) but no window system.
if(HRL_ENABLE_VULKAN)
    add_executable(hrl_test_vulkan vulkan_smoke.cpp)
    target_link_libraries(hrl_test_vulkan PRIVATE hrldll)
    add_test(NAME vulkan_smoke COMMAND hrl_test_vulkan WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME vulkan_smoke_no_raytracing COMMAND hrl_test_vulkan WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    # Validation layers turn every Vulkan misuse into a test failure when installed.
    set_tests_properties(vulkan_smoke PROPERTIES ENVIRONMENT "HRL_VULKAN_VALIDATION=1")
    set_tests_properties(vulkan_smoke_no_raytracing PROPERTIES ENVIRONMENT "HRL_VULKAN_VALIDATION=1;HRL_VULKAN_NO_RAYTRACING=1")
endif()
