robocopy "../build/" "python" "libhrldll.dll"

swig -python -c++ -outdir ./python -o ./python/hrl_wrap.cxx -I../src hrl.i
cd python
python setup.py build_ext --inplace --compiler=mingw32
cd ..
pause