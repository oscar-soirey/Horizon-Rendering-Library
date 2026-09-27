cd build
mingw32-make -j12

robocopy "." "../example/lib/" "libhrldll.dll.a"
robocopy "." "../example/build/" "libhrldll.dll"

robocopy "../src/" "../example/third_party/hrl/" "hrl.h"
robocopy "../src/" "../example/third_party/hrl/" "hrl_gl.h"
robocopy "../src/" "../example/third_party/hrl/" "hrl_vulkan.h"

cd ..
cd example/build
mingw32-make
example.exe
pause