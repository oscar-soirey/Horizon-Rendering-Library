@echo off
mkdir build-release 2>nul
cd build-release

cmake .. -G "MinGW Makefiles" ^
    -DCMAKE_BUILD_TYPE=Release

pause