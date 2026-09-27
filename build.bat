cd build
cmake -S .. -B . -DCOMPILE_VULKAN=ON
cmake --build . -j12
pause