cd build
mingw32-make

robocopy "." "../example/lib/" "libhrldll.dll.a"
robocopy "." "../example/build/" "libhrldll.dll"

cd ..
cd example/build
mingw32-make
example.exe