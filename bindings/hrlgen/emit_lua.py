"""LuaJIT FFI emitter : one module hrl.lua (no compilation).

    local hrl = require("hrl")       -- HRL_LIBRARY env var overrides the library path
    local scene = hrl.CreateScene()  -- names without the HRL_ prefix
    hrl.C.HRL_CreateScene()          -- raw ffi namespace

Wrappers : Lua strings in/out, tables for input arrays, out parameters
returned as extra values, variadic functions take one text argument.
"""
from .common import HEADER_NOTE, param_roles, sized_by, strip_prefix, clean_name


def ctext(t, name=""):
    s = ("const " if t.const else "") + t.base + " " + "*" * t.pointers + name
    return s.strip()


LUA_KW = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in",
          "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"}


def lname(n):
    n = clean_name(n)
    return n + "_" if n in LUA_KW else n


def cdef(api):
    consts = api.int_names()
    o = []
    for n, t in api.aliases.items():
        o.append("typedef %s;" % ctext(t, n))
    for s in api.structs:
        if s.opaque:
            o.append("typedef struct %s %s;" % (s.name, s.name))
    for e in api.enums:
        o.append("typedef enum { %s } %s;" % (", ".join("%s = %d" % (n, v) for n, v, _ in e.values), e.name))
    for s in api.structs:
        if s.opaque:
            continue
        fields = []
        for f in s.fields:
            arr = "[%s]" % consts.get(f.array, f.array) if f.array else ""
            fields.append("%s%s;" % (ctext(f.type, f.name), arr))
        o.append("typedef struct %s { %s } %s;" % (s.name, " ".join(fields), s.name))
    for c in api.callbacks:
        o.append("typedef %s (*%s)(%s);" % (ctext(c.ret), c.name, ", ".join(ctext(p.type, clean_name(p.name)) for p in c.params) or "void"))
    for f in api.functions:
        ps = [ctext(p.type, clean_name(p.name)) for p in f.params]
        if f.variadic:
            ps.append("...")
        o.append("%s %s(%s);" % (ctext(f.ret), f.name, ", ".join(ps) or "void"))
    return "\n".join(o)


def emit(api, headers):
    o = []
    w = o.append
    w("-- " + HEADER_NOTE.format(headers=", ".join(headers), version=api.version))
    w("-- LuaJIT FFI bindings of the Horizon Rendering Library.")
    w('local ffi = require("ffi")')
    w("ffi.cdef[[")
    w(cdef(api))
    w("]]")
    w("")
    w("local function load()")
    w('  local env = os.getenv("HRL_LIBRARY")')
    w("  if env and env ~= \"\" then return ffi.load(env) end")
    w('  local ok, lib = pcall(ffi.load, "hrldll")')
    w("  if ok then return lib end")
    w('  return ffi.load("hrl")')
    w("end")
    w("local C = load()")
    w("local M = { C = C, ffi = ffi, version = %r }" .replace("%r", '"%s"' % api.version))
    w("local keep = {}  -- callbacks passed to native code stay referenced here")
    w("")
    w("local function str(p) if p == nil then return nil end return ffi.string(p) end")
    w("local function arr(ct, t) if type(t) ~= \"table\" then return t, 0 end return ffi.new(ct .. \"[?]\", #t, t), #t end")
    w("")
    for c in api.constants:
        if c.kind == "string":
            w('M.%s = "%s"' % (c.name, c.value.replace('"', '\\"')))
        else:
            w("M.%s = %s" % (c.name, repr(c.value)))
    for e in api.enums:
        for n, v, _ in e.values:
            w("M.%s = %d" % (n, v))
    w("")
    for f in api.functions:
        roles = param_roles(api, f)
        sizes = sized_by(roles)
        args, pre, call, rets = [], [], [], []
        for i, (p, r) in enumerate(zip(f.params, roles)):
            n = lname(p.name)
            if r == "size":
                bufs = sizes.get(i, [])
                call.append("_n%d" % bufs[0] if bufs else "0")
                continue
            if f.variadic and i == len(f.params) - 1:
                call.append('"%s"')
                continue
            elem = ctext(type(p.type)(p.type.base, p.type.pointers - 1, False)) if p.type.pointers else None
            if r == "bytes":
                args.append(n)
                pre.append("local _n%d = #%s" % (i, n))
                call.append(n)
            elif r == "array":
                args.append(n)
                pre.append('local _p%d, _n%d = arr("%s", %s)' % (i, i, elem, n))
                call.append("_p%d" % i)
            elif r == "strings":
                args.append(n)
                pre.append("local _n%d = #%s" % (i, n))
                pre.append('local _p%d = ffi.new("const char*[?]", _n%d, %s)' % (i, i, n))
                call.append("_p%d" % i)
            elif r in ("out", "out_struct"):
                pre.append('local _o%d = ffi.new("%s[1]")' % (i, elem))
                call.append("_o%d" % i)
                rets.append(("tonumber(_o%d[0])" if r == "out" else "_o%d[0]") % i)
            elif r == "out_matrix":
                pre.append('local _o%d = ffi.new("float[16]")' % i)
                call.append("_o%d" % i)
                rets.append("_mat(_o%d)" % i)
            elif r == "out_string":
                pre.append('local _o%d = ffi.new("const char*[1]")' % i)
                call.append("_o%d" % i)
                rets.append("str(_o%d[0])" % i)
            elif r == "callback":
                args.append(n)
                pre.append('keep["%s"] = %s' % (f.name, n))
                call.append(n)
            else:
                args.append(n)
                call.append(n)
        if f.variadic:
            args.append("text")
            call.append("tostring(text)")
        rt = api.resolve(f.ret)
        res = []
        if not f.ret.is_void():
            res.append("str(_r)" if rt.base == "char" and rt.pointers == 1 else
                       "tonumber(_r)" if rt.pointers == 0 and (api.enum(rt.base) or rt.base in ("int64_t", "uint64_t", "size_t")) else "_r")
        res += rets
        w("function M.%s(%s)" % (strip_prefix(f.name), ", ".join(args)))
        for l in pre:
            w("  " + l)
        callexpr = "C.%s(%s)" % (f.name, ", ".join(call))
        if f.ret.is_void():
            w("  " + callexpr)
        else:
            w("  local _r = " + callexpr)
        if res:
            w("  return " + ", ".join(res))
        w("end")
    w("")
    o.insert(o.index("local function str(p) if p == nil then return nil end return ffi.string(p) end"),
             "local function _mat(p) local t = {} for k = 0, 15 do t[k + 1] = p[k] end return t end")
    w("return M")
    return {"hrl.lua": "\n".join(o) + "\n"}
