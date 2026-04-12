%module hrl
%{
#include "../src/hrl.h"
%}

%include <stdint.i>
%ignore HRL_CheckErrors;

#ifdef SWIGJAVA
    %include "hrl_java.i"
#endif
#ifdef SWIGPYTHON
    %include "hrl_python.i"
#endif
#ifdef SWIGCSHARP
    %include "hrl_csharp.i"
#endif

%include "../src/hrl.h"

#ifdef SWIGPYTHON
%pythoncode %{
def HRL_GetLastError():
    import ctypes
    lib = ctypes.cdll.LoadLibrary("libhrldll.dll")
    lib.HRL_GetLastError.restype = ctypes.c_int
    detail = ctypes.c_char_p()
    severity = ctypes.c_int()
    err = lib.HRL_GetLastError(ctypes.byref(detail), ctypes.byref(severity))
    return err, detail.value.decode() if detail.value else "", severity.value
%}
#endif