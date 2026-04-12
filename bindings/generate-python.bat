robocopy "../build/" "python" "libhrldll.dll"

swig -python -c++ -outdir ./python -o ./python/hrl_wrap.cxx -I../src hrl.i
cd python
py -3.9 setup.py build_ext --inplace --compiler=mingw32
cd ..
pause