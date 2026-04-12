%include <typemaps.i>
%include <carrays.i>
%array_class(float, FloatArray);
/* int* OUTPUT */
%apply int* OUTPUT { int* _width, int* _height };
%apply int* OUTPUT { HRL_Severity* _severity };
/* Callback */
%typemap(in) HRL_ErrorCallback {
    $1 = NULL;
}
/* Loader */
%typemap(in) void* _loader {
    $1 = PyLong_AsVoidPtr($input);
}
/* Binary buffer */
%typemap(in) char const *_data {
    if (!PyBytes_Check($input)) {
        PyErr_SetString(PyExc_TypeError, "Expected bytes object");
        SWIG_fail;
    }
    $1 = PyBytes_AS_STRING($input);
}
/* Ignorer HRL_GetLastError — rewrappé manuellement en dessous */
%ignore HRL_GetLastError;

%pythoncode %{
HRL_SPRITE_SHADER = 0xFFFFFFFF
HRL_MESH_2D_SHADER = 0xFFFFFFFF - 1
HRL_MESH_3D_SHADER = 0xFFFFFFFF - 2
HRL_DEBUG_SHADER = 0xFFFFFFFF - 3
HRL_DEFAULT_POST_PROCESS_SHADER = 0xFFFFFFFF - 4
%}