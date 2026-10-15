"""Python emitter : pure ctypes package (no compilation, any Python 3.8+)."""
from .common import HEADER_NOTE, param_roles, sized_by, clean_name, enum_of
from .parser import CType

_CTYPES = {
    "void": "None", "int": "ctypes.c_int", "unsigned int": "ctypes.c_uint", "float": "ctypes.c_float",
    "double": "ctypes.c_double", "char": "ctypes.c_char", "unsigned char": "ctypes.c_ubyte",
    "int8_t": "ctypes.c_int8", "uint8_t": "ctypes.c_uint8", "int16_t": "ctypes.c_int16", "uint16_t": "ctypes.c_uint16",
    "int32_t": "ctypes.c_int32", "uint32_t": "ctypes.c_uint32", "int64_t": "ctypes.c_int64", "uint64_t": "ctypes.c_uint64",
    "size_t": "ctypes.c_size_t", "bool": "ctypes.c_bool", "short": "ctypes.c_short",
}


def ctype_expr(api, t, in_struct=False):
    """ctypes type expression of a C type (typedef names are kept as aliases)."""
    if api.callback(t.base) and t.pointers == 0:
        return t.base
    if t.pointers == 0:
        if t.base in _CTYPES:
            return _CTYPES[t.base]
        if api.enum(t.base):
            return "ctypes.c_int"
        if t.base in api.aliases or api.struct(t.base):
            return t.base
        raise ValueError("unknown type " + repr(t))
    if t.base == "void" and t.pointers == 1:
        return "ctypes.c_void_p"
    if t.base == "char" and t.pointers == 1:
        return "ctypes.c_char_p"
    inner = ctype_expr(api, CType(t.base, t.pointers - 1, t.const), in_struct)
    if inner == "None":
        inner = "ctypes.c_void_p"
    s = api.struct(t.base)
    if s and s.opaque and t.pointers == 1:
        return "ctypes.c_void_p"
    return "ctypes.POINTER(%s)" % inner


def py_doc(f, extra):
    lines = []
    if f.doc:
        lines += f.doc.splitlines()
    if extra:
        if lines:
            lines.append("")
        lines += extra
    text = "\n".join(lines).replace("\\", "\\\\").replace('"""', '\\"\\"\\"')
    return text


def emit(api, headers):
    o = []
    w = o.append
    w('"""HRL (Horizon Rendering Library) for Python.')
    w("")
    w(HEADER_NOTE.format(headers=", ".join(headers), version=api.version))
    w("")
    w("Every C function of hrl.h is available with the same name. Python")
    w("values are converted automatically :")
    w("  * str for const char* (UTF-8), bytes-like objects for binary data")
    w("    (the size argument is filled in for you),")
    w("  * sequences for input arrays (vertices, indices...),")
    w("  * output pointers become return values (a tuple when there are several),")
    w("  * Python callables for callbacks (kept alive by the module).")
    w("The raw ctypes functions are in hrl.raw.")
    w('"""')
    w("import ctypes")
    w("import enum as _enum")
    w("")
    w("from ._loader import load_library as _load_library")
    w("")
    w("_lib = _load_library()")
    w("")
    w("")
    w("class _Raw:")
    w('    """Raw ctypes functions (exact C signatures)."""')
    w("")
    w("")
    w("raw = _Raw()")
    w("")
    w("")
    w("def _text(value):")
    w("    if value is None or isinstance(value, bytes):")
    w("        return value")
    w("    if isinstance(value, str):")
    w("        return value.encode('utf-8')")
    w("    raise TypeError('expected str or bytes, got %s' % type(value).__name__)")
    w("")
    w("")
    w("def _from_text(value):")
    w("    return value.decode('utf-8', 'replace') if value is not None else None")
    w("")
    w("")
    w("def _buffer(value):")
    w("    \"\"\"bytes-like -> (pointer, size, keepalive)\"\"\"")
    w("    if value is None:")
    w("        return None, 0, None")
    w("    if isinstance(value, str):")
    w("        value = value.encode('utf-8')")
    w("    if isinstance(value, bytes):")
    w("        return ctypes.cast(ctypes.c_char_p(value), ctypes.c_void_p), len(value), value")
    w("    view = memoryview(value).cast('B')")
    w("    if view.readonly:")
    w("        data = bytes(view)")
    w("        return ctypes.cast(ctypes.c_char_p(data), ctypes.c_void_p), len(data), data")
    w("    array = (ctypes.c_ubyte * len(view)).from_buffer(view)")
    w("    return ctypes.cast(array, ctypes.c_void_p), len(view), array")
    w("")
    w("")
    w("def _array(ctype, values):")
    w("    \"\"\"sequence / ctypes array / buffer -> (ctypes array, count)\"\"\"")
    w("    if values is None:")
    w("        return None, 0")
    w("    if isinstance(values, ctypes.Array):")
    w("        return values, len(values)")
    w("    try:")
    w("        view = memoryview(values)")
    w("        if view.nbytes % ctypes.sizeof(ctype) == 0 and not isinstance(values, (list, tuple)):")
    w("            count = view.nbytes // ctypes.sizeof(ctype)")
    w("            data = bytearray(view.cast('B'))")
    w("            return (ctype * count).from_buffer(data), count")
    w("    except TypeError:")
    w("        pass")
    w("    values = list(values)")
    w("    return (ctype * len(values))(*values), len(values)")
    w("")
    w("")
    w("_keepalive = {}")
    w("")

    # constants -------------------------------------------------------------
    w("# " + "-" * 74)
    w("# Constants")
    w("# " + "-" * 74)
    for c in api.constants:
        if c.kind == "string":
            w("%s = %r" % (c.name, c.value))
        else:
            w("%s = %r" % (c.name, c.value))
    w("")

    # typedefs ---------------------------------------------------------------
    for name, t in api.aliases.items():
        w("%s = %s" % (name, ctype_expr(api, t)))
    w("")

    # enums --------------------------------------------------------------------
    w("# " + "-" * 74)
    w("# Enumerations (IntEnum classes; the values are also module constants)")
    w("# " + "-" * 74)
    w("")
    w("class _Enum(_enum.IntEnum):")
    w('    """IntEnum accepting values unknown to this binding (returned as pseudo-members)."""')
    w("    @classmethod")
    w("    def _missing_(cls, value):")
    w("        if not isinstance(value, int):")
    w("            return None")
    w("        m = int.__new__(cls, value)")
    w("        m._name_ = '%s(%d)' % (cls.__name__, value)")
    w("        m._value_ = value")
    w("        return m")
    for e in api.enums:
        w("")
        w("class %s(_Enum):" % e.name)
        if e.doc:
            w('    """%s"""' % e.doc.replace("\\", "\\\\").replace('"""', "'''"))
        for n, v, d in e.values:
            w("    %s = %d" % (n, v))
        w("")
        for n, v, d in e.values:
            w("%s = %s.%s" % (n, e.name, n))
    w("")

    # structs (two passes : forward declarations then fields) ---------------
    w("# " + "-" * 74)
    w("# Structures")
    w("# " + "-" * 74)
    for s in api.structs:
        w("")
        w("class %s(ctypes.Structure):" % s.name)
        if s.doc:
            w('    """%s"""' % s.doc.replace('"""', "'''"))
        w("    pass")
    for s in api.structs:
        if s.opaque:
            continue
        w("")
        w("%s._fields_ = [" % s.name)
        for f in s.fields:
            t = ctype_expr(api, f.type, True)
            if f.array:
                n = f.array
                consts = api.int_names()
                n = str(consts.get(n, n))
                t = "(%s * %s)" % (t, n)
            w("    (%r, %s)," % (f.name, t))
        w("]")
    w("")

    # callbacks ----------------------------------------------------------------
    w("# " + "-" * 74)
    w("# Callback types (ctypes.CFUNCTYPE)")
    w("# " + "-" * 74)
    for c in api.callbacks:
        args = [ctype_expr(api, c.ret) if not c.ret.is_void() else "None"]
        for p in c.params:
            a = ctype_expr(api, p.type)
            args.append("ctypes.c_char_p" if a == "ctypes.c_char_p" else a)
        w("%s = ctypes.CFUNCTYPE(%s)" % (c.name, ", ".join(args)))
    w("")

    # functions -----------------------------------------------------------------
    w("# " + "-" * 74)
    w("# Functions")
    w("# " + "-" * 74)
    for f in api.functions:
        roles = param_roles(api, f)
        sizes = sized_by(roles)
        argtypes = []
        for p, r in zip(f.params, roles):
            a = ctype_expr(api, p.type)
            if r == "bytes":
                a = "ctypes.c_void_p"
            argtypes.append(a)
        restype = ctype_expr(api, f.ret) if not f.ret.is_void() else "None"
        w("")
        w("try:")
        w("    _f = _lib.%s" % f.name)
        w("    _f.argtypes = [%s]" % ", ".join(argtypes))
        if f.variadic:
            w("    _f.argtypes = None  # variadic")
        w("    _f.restype = %s" % restype)
        w("    setattr(raw, %r, _f)" % f.name)
        w("except AttributeError:  # not exported by this build (e.g. other backend)")
        w("    pass")
        w("")

        # wrapper
        args = []
        pre = []
        call = []
        outs = []
        post_ret = None
        extra = []
        out_names = []
        for i, (p, r) in enumerate(zip(f.params, roles)):
            n = clean_name(p.name)
            if n in ("lambda", "type", "id", "len", "format", "min", "max", "object", "input", "filter", "map", "str", "range", "hash", "property"):
                n = n + "_"
            t = api.resolve(p.type)
            if r == "size":
                buffers = sizes.get(i, [])
                call.append("_n%d" % buffers[0] if buffers else "0")
                continue
            if f.variadic and i == len(f.params) - 1:
                call.append("b'%s'")  # printf format : the text is passed as is
                continue
            if r == "value":
                args.append(n)
                # variadic functions have no argtypes : convert explicitly
                call.append("%s(%s)" % (ctype_expr(api, p.type), n) if f.variadic else n)
            elif r == "string":
                args.append(n)
                call.append("_text(%s)" % n)
            elif r == "bytes":
                args.append(n)
                pre.append("_p%d, _n%d, _k%d = _buffer(%s)" % (i, i, i, n))
                call.append("_p%d" % i)
            elif r == "array":
                args.append(n)
                inner = ctype_expr(api, CType(t.base))
                pre.append("_a%d, _n%d = _array(%s, %s)" % (i, i, inner, n))
                call.append("_a%d" % i)
            elif r == "strings":
                args.append(n)
                pre.append("_s%d = [_text(s) for s in (%s or [])]" % (i, n))
                pre.append("_a%d = (ctypes.c_char_p * len(_s%d))(*_s%d)" % (i, i, i))
                pre.append("_n%d = len(_s%d)" % (i, i))
                call.append("_a%d" % i)
            elif r == "out":
                inner = ctype_expr(api, CType(t.base))
                pre.append("_o%d = %s()" % (i, inner))
                call.append("ctypes.byref(_o%d)" % i)
                e = enum_of(api, CType(t.base))
                outs.append(("%s(_o%d.value)" % (e.name, i)) if e else "_o%d.value" % i)
                out_names.append(n)
            elif r == "out_matrix":
                pre.append("_o%d = (ctypes.c_float * 16)()" % i)
                call.append("_o%d" % i)
                outs.append("list(_o%d)" % i)
                out_names.append(n + " (16 floats, column major)")
            elif r == "out_string":
                pre.append("_o%d = ctypes.c_char_p()" % i)
                call.append("ctypes.byref(_o%d)" % i)
                outs.append("_from_text(_o%d.value)" % i)
                out_names.append(n)
            elif r == "out_struct":
                pre.append("_o%d = %s()" % (i, t.base))
                call.append("ctypes.byref(_o%d)" % i)
                outs.append("_o%d" % i)
                out_names.append(n)
            elif r == "callback":
                args.append(n)
                pre.append("_cb%d = %s(%s) if %s is not None and not isinstance(%s, %s) else %s" % (i, p.type.base, n, n, n, p.type.base, n))
                key_parts = [repr(f.name)] + [clean_name(q.name) for q, rr in zip(f.params, roles) if rr == "value"][:1]
                pre.append("_keepalive[(%s)] = _cb%d" % (", ".join(key_parts) + ("," if len(key_parts) == 1 else ""), i))
                call.append("_cb%d" % i)
            else:  # pointer
                args.append(n)
                call.append(n)
        if f.variadic:
            # printf style : the text is passed through "%s"
            args.append("text")
            call.append("_text(text)")
        ret_conv = None
        rt = api.resolve(f.ret)
        e = enum_of(api, f.ret)
        if rt.base == "char" and rt.pointers == 1:
            ret_conv = "_from_text(_r)"
        elif e:
            ret_conv = "%s(_r)" % e.name
        sig = ", ".join(args)
        w("def %s(%s):" % (f.name, sig))
        if out_names:
            extra.append("Returns: " + ", ".join((["result"] if not f.ret.is_void() else []) + out_names))
        d = py_doc(f, extra)
        if d:
            w('    """%s"""' % d)
        for line in pre:
            w("    " + line)
        w("    _r = _lib.%s(%s)" % (f.name, ", ".join(call)))
        results = []
        if not f.ret.is_void():
            results.append(ret_conv or "_r")
        results += outs
        if len(results) == 1:
            w("    return %s" % results[0])
        elif results:
            w("    return (%s)" % ", ".join(results))
    w("")
    w("")
    w("__all__ = [n for n in dir() if n.startswith('HRL_')] + ['raw']")
    return "\n".join(o) + "\n"


LOADER = '''"""Locates and loads the HRL shared library."""
import ctypes
import os
import sys

_NAMES = {
    "win32": ["hrldll.dll", "libhrldll.dll", "hrl.dll"],
    "darwin": ["libhrldll.dylib", "libhrl.dylib"],
}
_DEFAULT = ["libhrldll.so", "libhrl.so"]


def load_library():
    """HRL_LIBRARY (full path) > the package folder > the current folder >
    the system search path."""
    explicit = os.environ.get("HRL_LIBRARY")
    if explicit:
        return _load(explicit)
    names = _NAMES.get(sys.platform, _DEFAULT)
    here = os.path.dirname(os.path.abspath(__file__))
    if sys.platform == "win32" and hasattr(os, "add_dll_directory"):
        os.add_dll_directory(here)  # dependencies shipped next to the DLL (MinGW runtime...)
    for folder in (here, os.getcwd()):
        for n in names:
            path = os.path.join(folder, n)
            if os.path.exists(path):
                return _load(path)
    last = None
    for n in names:
        try:
            return _load(n)
        except OSError as e:
            last = e
    raise OSError("HRL shared library not found (set HRL_LIBRARY to its path): %s" % last)


def _load(path):
    if sys.platform == "win32":
        return ctypes.CDLL(path, winmode=0) if sys.version_info >= (3, 8) else ctypes.CDLL(path)
    return ctypes.CDLL(path)
'''

PYPROJECT = '''[build-system]
requires = ["setuptools>=61"]
build-backend = "setuptools.build_meta"

[project]
name = "hrl"
version = "{version}"
description = "Python bindings of the Horizon Rendering Library (ctypes, generated)"
requires-python = ">=3.8"
license = {{text = "Apache-2.0"}}

[tool.setuptools]
packages = ["hrl"]

[tool.setuptools.package-data]
hrl = ["*.dll", "*.so", "*.dylib", "py.typed"]
'''
