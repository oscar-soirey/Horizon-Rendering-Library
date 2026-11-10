@echo off
rem Builds HRL with its examples and starts the Vulkan example.
cd /d "%~dp0" || exit /b 1
cmake -S . -B build -DHRL_BUILD_EXAMPLES=ON || goto :error
cmake --build build --config Release -j12 || goto :error
rem Visual Studio puts the binaries in bin\Release, single-config generators in bin.
if exist "build\bin\Release\hrl_vulkan_pbr.exe" (cd build\bin\Release) else (cd build\bin)
hrl_vulkan_pbr.exe
pause
exit /b 0
:error
echo Build failed.
pause
exit /b 1
