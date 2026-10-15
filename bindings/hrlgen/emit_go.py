"""Go emitter : cgo package "hrl".

Types : HRL_id -> ID, HRL_ELightType -> ELightType, HRL_Vertex3D -> Vertex3D.
Functions : HRL_CreateScene -> CreateScene (strings, slices, multiple results).
Callbacks : Go funcs, kept in a registry (the C user_data is not used).
"""
import re

from .common import HEADER_NOTE, param_roles, sized_by, clean_name, strip_prefix
from .parser import CType

_GO = {
    "int": "int32", "unsigned int": "uint32", "float": "float32", "double": "float64", "char": "byte",
    "unsigned char": "uint8", "int8_t": "int8", "uint8_t": "uint8", "int16_t": "int16", "uint16_t": "uint16",
    "int32_t": "int32", "uint32_t": "uint32", "int64_t": "int64", "uint64_t": "uint64", "size_t": "uintptr",
    "bool": "bool", "short": "int16",
}
_KEYWORDS = {"type", "func", "range", "map", "chan", "go", "select", "case", "default", "package", "import",
             "var", "const", "struct", "interface", "return", "break", "continue", "for", "if", "else", "switch",
             "defer", "fallthrough", "goto", "string", "len", "cap", "new", "make", "error", "nil", "true", "false"}


def go_type_name(c):
    n = strip_prefix(c)
    return {"id": "ID", "uint": "Uint", "VoxelType": "VoxelType"}.get(n, n)


def go_name(n):
    n = clean_name(n)
    parts = n.split("_")
    n = parts[0] + "".join(p[:1].upper() + p[1:] for p in parts[1:])
    n = n[:1].lower() + n[1:]
    return n + "_" if n in _KEYWORDS else n


def go_field(n):
    return n[:1].upper() + n[1:]


def go_type(api, t):
    """Go type with the same memory layout as the C type."""
    if t.pointers:
        if t.base == "void" or (api.struct(t.base) and api.struct(t.base).opaque) or t.pointers > 1:
            return "unsafe.Pointer"
        if t.base == "char":
            return "*byte"
        return "*" + go_type(api, CType(t.base, t.pointers - 1))
    if t.base in api.aliases:
        return go_type_name(t.base)
    if t.base in _GO:
        return _GO[t.base]
    if api.enum(t.base) or api.struct(t.base) or api.callback(t.base):
        return go_type_name(t.base)
    raise ValueError("Go: unknown type %r" % t)


def c_ref(api, t):
    """cgo spelling of a C type."""
    if t.pointers:
        if t.base == "void":
            return "unsafe.Pointer"
        return "*" * t.pointers + c_ref(api, CType(t.base))
    m = {"unsigned int": "C.uint", "unsigned char": "C.uchar", "size_t": "C.size_t", "int": "C.int", "float": "C.float",
         "double": "C.double", "char": "C.char", "uint32_t": "C.uint32_t", "uint64_t": "C.uint64_t", "uint8_t": "C.uint8_t",
         "int32_t": "C.int32_t", "int64_t": "C.int64_t"}
    if t.base in m:
        return m[t.base]
    return "C." + t.base


def go_doc(doc, name):
    if not doc:
        return []
    lines = doc.splitlines()
    out = ["// %s : %s" % (name, lines[0])]
    out += ["// " + l if l else "//" for l in lines[1:]]
    return out


def emit(api, headers):
    files = {}
    note = HEADER_NOTE.format(headers=", ".join(headers), version=api.version)
    consts = api.int_names()

    o = []
    w = o.append
    w("// Package hrl : Go bindings of the Horizon Rendering Library.")
    w("//")
    w("// " + note)
    w("//")
    w("// Link : set CGO_LDFLAGS=\"-L<folder of the HRL library>\" (library hrldll,")
    w("// change with CGO_LDFLAGS=-l<name> and the hrl_custom_lib build tag).")
    w("package hrl")
    w("")
    w("/*")
    w("#cgo CFLAGS: -I${SRCDIR}/../../../src -DHRL_NO_DLL")
    w("#cgo !hrl_custom_lib LDFLAGS: -lhrldll")
    w("#include <stdlib.h>")
    w('#include "hrl.h"')
    w('#include "hrl_gl.h"')
    w('#include "hrl_vulkan.h"')
    w("")
    for c in api.callbacks:
        params = ", ".join(("const " if p.type.const else "") + p.type.base + "*" * p.type.pointers for p in c.params)
        w("%s hrlGo%s(%s);" % (("const " if c.ret.const else "") + c.ret.base + "*" * c.ret.pointers, c.name, params))
    for f in api.functions:
        if f.variadic:
            fixed = f.params[:-1]
            params = ", ".join("%s %s" % (("const " if p.type.const else "") + p.type.base + "*" * p.type.pointers, clean_name(p.name)) for p in fixed)
            args = ", ".join(clean_name(p.name) for p in fixed)
            w("static %s hrl_%s(%s, const char* text) { return %s(%s, \"%%s\", text); }" % (
                f.ret.base + "*" * f.ret.pointers, f.name, params, f.name, args))
    w("*/")
    w('import "C"')
    w("")
    w("import (")
    w('\t"sync"')
    w('\t"unsafe"')
    w(")")
    w("")
    # aliases
    for name, t in api.aliases.items():
        w("// %s is %s." % (go_type_name(name), name))
        w("type %s = %s" % (go_type_name(name), _GO[api.resolve(CType(name)).base]))
    w("")
    # constants
    w("const (")
    for c in api.constants:
        if c.kind == "string":
            w('\t%s = "%s"' % (c.name, c.value))
        elif c.kind == "uint":
            w("\t%s %s = %d" % (c.name, go_type_name(c.ctype) if c.ctype in api.aliases else "uint32", c.value))
        elif c.kind == "float":
            w("\t%s = %r" % (c.name, c.value))
        else:
            w("\t%s = %d" % (c.name, c.value))
    w(")")
    w("")
    for e in api.enums:
        o.extend(go_doc(e.doc, go_type_name(e.name)))
        w("type %s int32" % go_type_name(e.name))
        w("")
        w("const (")
        for n, v, d in e.values:
            w("\t%s %s = %d" % (n, go_type_name(e.name), v))
        w(")")
        w("")
    # structs (same layout as C, checked at compile time)
    for s in api.structs:
        gn = go_type_name(s.name)
        o.extend(go_doc(s.doc, gn))
        if s.opaque:
            w("type %s struct{ _ [0]byte }" % gn)
            w("")
            continue
        w("type %s struct {" % gn)
        for f in s.fields:
            t = go_type(api, f.type)
            if f.array:
                t = "[%s]%s" % (consts.get(f.array, f.array), t)
            w("\t%s %s" % (go_field(f.name), t))
        w("}")
        w("")
        w("// layout check : fails to compile if Go and C disagree")
        w("const _ = uint(unsafe.Sizeof(%s{})-C.sizeof_%s) + uint(C.sizeof_%s-unsafe.Sizeof(%s{}))" % (gn, s.name, s.name, gn))
        w("")
    # callbacks
    for c in api.callbacks:
        gn = go_type_name(c.name)
        params = ", ".join("%s %s" % (go_name(p.name), "string" if (api.resolve(p.type).base == "char" and p.type.pointers == 1) else go_type(api, p.type))
                           for p in c.params if not (p.type.base == "void" and clean_name(p.name) == "user_data"))
        ret = "" if c.ret.is_void() else " " + go_type(api, c.ret)
        o.extend(go_doc(c.doc, gn))
        w("type %s func(%s)%s" % (gn, params, ret))
        w("")
    w("var callbacks sync.Map // key -> Go func")
    w("")
    w("func cBool(b bool) C.int {")
    w("\tif b {")
    w("\t\treturn 1")
    w("\t}")
    w("\treturn 0")
    w("}")
    w("")
    w("func goString(p *C.char) string {")
    w("\tif p == nil {")
    w('\t\treturn ""')
    w("\t}")
    w("\treturn C.GoString(p)")
    w("}")
    w("")
    # functions
    for f in api.functions:
        roles = param_roles(api, f)
        sizes = sized_by(roles)
        sig = []
        pre = []
        call = []
        outs = []
        out_types = []
        for i, (p, r) in enumerate(zip(f.params, roles)):
            n = go_name(p.name)
            t = api.resolve(p.type)
            cr = c_ref(api, p.type)
            if r == "size":
                bufs = sizes.get(i, [])
                call.append("%s(_n%d)" % (cr, bufs[0]) if bufs else "0")
                continue
            if f.variadic and i == len(f.params) - 1:
                continue
            if r == "value":
                gt = go_type(api, p.type)
                sig.append("%s %s" % (n, gt))
                call.append("%s(%s)" % (cr, n))
            elif r == "string":
                sig.append("%s string" % n)
                pre.append("_s%d := C.CString(%s)" % (i, n))
                pre.append("defer C.free(unsafe.Pointer(_s%d))" % i)
                call.append("_s%d" % i)
            elif r == "bytes":
                sig.append("%s []byte" % n)
                pre.append("_n%d := len(%s)" % (i, n))
                pre.append("var _p%d unsafe.Pointer" % i)
                pre.append("if _n%d > 0 {" % i)
                pre.append("\t_p%d = C.CBytes(%s)" % (i, n))
                pre.append("\tdefer C.free(_p%d)" % i)
                pre.append("}")
                call.append("(%s)(_p%d)" % (cr, i) if cr != "unsafe.Pointer" else "_p%d" % i)
            elif r == "array":
                elem = go_type(api, CType(t.base))
                sig.append("%s []%s" % (n, elem))
                pre.append("_n%d := len(%s)" % (i, n))
                pre.append("var _p%d unsafe.Pointer" % i)
                pre.append("if _n%d > 0 {" % i)
                pre.append("\t_p%d = unsafe.Pointer(&%s[0])" % (i, n))
                pre.append("}")
                call.append("(%s)(_p%d)" % (cr, i))
            elif r == "strings":
                sig.append("%s []string" % n)
                pre.append("_n%d := len(%s)" % (i, n))
                pre.append("_a%d := C.malloc(C.size_t(_n%d+1) * C.size_t(unsafe.Sizeof(uintptr(0))))" % (i, i))
                pre.append("defer C.free(_a%d)" % i)
                pre.append("_v%d := unsafe.Slice((**C.char)(_a%d), _n%d+1)" % (i, i, i))
                pre.append("for k, s := range %s {" % n)
                pre.append("\t_v%d[k] = C.CString(s)" % i)
                pre.append("\tdefer C.free(unsafe.Pointer(_v%d[k]))" % i)
                pre.append("}")
                call.append("(%s)(_a%d)" % (cr, i))
            elif r == "out":
                elem = go_type(api, CType(t.base))
                pre.append("var _o%d %s" % (i, c_ref(api, CType(t.base))))
                call.append("&_o%d" % i)
                outs.append("%s(_o%d)" % (elem, i))
                out_types.append(elem)
            elif r == "out_matrix":
                pre.append("var _o%d [16]float32" % i)
                call.append("(*C.float)(unsafe.Pointer(&_o%d[0]))" % i)
                outs.append("_o%d" % i)
                out_types.append("[16]float32")
            elif r == "out_string":
                pre.append("var _o%d *C.char" % i)
                call.append("&_o%d" % i)
                outs.append("goString(_o%d)" % i)
                out_types.append("string")
            elif r == "out_struct":
                elem = go_type(api, CType(t.base))
                pre.append("var _o%d %s" % (i, elem))
                call.append("(%s)(unsafe.Pointer(&_o%d))" % (cr, i))
                outs.append("_o%d" % i)
                out_types.append(elem)
            elif r == "callback":
                cb = p.type.base
                gn = go_type_name(cb)
                sig.append("%s %s" % (n, gn))
                ids = [go_name(q.name) for q, rr in zip(f.params, roles) if rr == "value"]
                key = '"%s"' % cb if not ids else 'callbackKey{"%s", uint64(%s)}' % (cb, ids[0])
                pre.append("var _c%d C.%s" % (i, cb))
                pre.append("if %s != nil {" % n)
                pre.append("\tcallbacks.Store(%s, %s)" % (key, n))
                pre.append("\t_c%d = C.%s(C.hrlGo%s)" % (i, cb, cb))
                pre.append("} else {")
                pre.append("\tcallbacks.Delete(%s)" % key)
                pre.append("}")
                call.append("_c%d" % i)
            else:  # pointer
                if p.type.base == "void" and clean_name(p.name) in ("user_data",) and any(rr == "callback" for rr in roles):
                    call.append("nil")
                    continue
                gt = go_type(api, p.type)
                sig.append("%s %s" % (n, gt))
                call.append("(%s)(unsafe.Pointer(%s))" % (cr, n) if cr != "unsafe.Pointer" else n)
        if f.variadic:
            sig.append("text string")
            pre.append("_text := C.CString(text)")
            pre.append("defer C.free(unsafe.Pointer(_text))")
            call.append("_text")
        rt = api.resolve(f.ret)
        ret_types = []
        ret_expr = []
        if not f.ret.is_void():
            if rt.base == "char" and rt.pointers == 1:
                ret_types.append("string")
                ret_expr.append("goString(_r)")
            else:
                gt = go_type(api, f.ret)
                ret_types.append(gt)
                ret_expr.append(("(%s)(unsafe.Pointer(_r))" if rt.pointers else "%s(_r)") % gt)
        ret_types += out_types
        ret_expr += outs
        name = strip_prefix(f.name)
        o.extend(go_doc(f.doc, name))
        ret = ""
        if len(ret_types) == 1:
            ret = " " + ret_types[0]
        elif ret_types:
            ret = " (" + ", ".join(ret_types) + ")"
        w("func %s(%s)%s {" % (name, ", ".join(sig), ret))
        for l in pre:
            w("\t" + l)
        target = ("C.hrl_" + f.name) if f.variadic else "C." + f.name
        if f.ret.is_void():
            w("\t%s(%s)" % (target, ", ".join(call)))
        else:
            w("\t_r := %s(%s)" % (target, ", ".join(call)))
        if ret_expr:
            w("\treturn " + ", ".join(ret_expr))
        w("}")
        w("")
    files["hrl/hrl.go"] = "\n".join(o) + "\n"

    # callbacks.go (//export : the preamble may only declare) ------------------
    o = []
    w = o.append
    w("// " + note)
    w("package hrl")
    w("")
    w("/*")
    w('#include "hrl.h"')
    w('#include "hrl_vulkan.h"')
    w("*/")
    w('import "C"')
    w("")
    w('import "unsafe"')
    w("")
    w("type callbackKey struct {")
    w("\tkind string")
    w("\tid   uint64")
    w("}")
    w("")
    w("var _ = unsafe.Pointer(nil)")
    w("")
    for c in api.callbacks:
        gn = go_type_name(c.name)
        params = ", ".join("%s %s" % (go_name(p.name), c_ref(api, p.type)) for p in c.params)
        ret = "" if c.ret.is_void() else " " + c_ref(api, c.ret)
        w("//export hrlGo%s" % c.name)
        w("func hrlGo%s(%s)%s {" % (c.name, params, ret))
        idp = [p for p in c.params if api.resolve(p.type).base == "uint32_t" and p.type.pointers == 0]
        key = 'callbackKey{"%s", uint64(%s)}' % (c.name, go_name(idp[0].name)) if idp else '"%s"' % c.name
        args = []
        for p in c.params:
            if p.type.base == "void" and clean_name(p.name) == "user_data":
                continue
            t = api.resolve(p.type)
            gt = go_type(api, p.type)
            if t.base == "char" and t.pointers == 1:
                args.append("C.GoString(%s)" % go_name(p.name))
            elif t.pointers:
                args.append("(%s)(unsafe.Pointer(%s))" % (gt, go_name(p.name)) if gt != "unsafe.Pointer" else go_name(p.name))
            else:
                args.append("%s(%s)" % (gt, go_name(p.name)))
        # error callback : char* -> string
        w("\tv, ok := callbacks.Load(%s)" % key)
        w("\tif !ok {")
        w("\t\treturn" + ("" if c.ret.is_void() else " 0"))
        w("\t}")
        
        call = "v.(%s)(%s)" % (gn, ", ".join(args))
        if c.ret.is_void():
            w("\t" + call)
        else:
            w("\treturn %s(%s)" % (c_ref(api, c.ret), call))
        w("}")
        w("")
    files["hrl/callbacks.go"] = "\n".join(o) + "\n"
    files["go.mod"] = "module github.com/oscar-soirey/hrl/bindings/go\n\ngo 1.21\n"
    return files
