cmake -S . -B build -DHRL_ENABLE_VULKAN=ON -DHRL_BUILD_EXAMPLES=ON -DHRL_BUILD_TESTS=ON
cmake --build build --config Release -j12
pause
