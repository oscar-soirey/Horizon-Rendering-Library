"""Rust emitter : crate "hrl" (raw FFI in hrl::sys, safe wrappers at the root)."""
from .common import HEADER_NOTE, param_roles, sized_by, clean_name, enum_of, snake
from .parser import CType

_RS = {
    "void": "c_void", "int": "c_int", "unsigned int": "c_uint", "float": "f32", "double": "f64",
    "char": "c_char", "unsigned char": "c_uchar", "int8_t": "i8", "uint8_t": "u8", "int16_t": "i16",
    "uint16_t": "u16", "int32_t": "i32", "uint32_t": "u32", "int64_t": "i64", "uint64_t": "u64",
    "size_t": "usize", "bool": "bool", "short": "std::os::raw::c_short",
}
_KEYWORDS = {"type", "ref", "match", "loop", "move", "fn", "impl", "mod", "use", "where", "self", "super", "crate",
             "box", "in", "for", "if", "else", "let", "mut", "pub", "return", "static", "struct", "trait", "true",
             "false", "unsafe", "while", "dyn", "abstract", "final", "override", "macro", "async", "await", "const", "enum"}


def rs_name(n):
    n = clean_name(n)
    n = n[0].lower() + n[1:] if n[:1].isupper() and not n.isupper() else n
    import re
    n = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", n).lower()
    return n + "_" if n in _KEYWORDS else n


def rs_type(api, t):
    if api.callback(t.base) and t.pointers == 0:
        return t.base
    if t.pointers:
        inner = rs_type(api, CType(t.base, t.pointers - 1, t.const))
        return ("*const " if t.const else "*mut ") + inner
    if t.base in _RS:
        return _RS[t.base]
    if t.base in api.aliases or api.enum(t.base) or api.struct(t.base):
        return t.base
    raise ValueError("Rust: unknown type %r" % t)


def rs_doc(doc, indent, inner=False):
    if not doc:
        return []
    mark = "//!" if inner else "///"
    return [indent + mark + (" " + l if l else "") for l in doc.replace("\t", "    ").splitlines()]


def emit(api, headers):
    files = {}
    note = HEADER_NOTE.format(headers=", ".join(headers), version=api.version)
    consts = api.int_names()

    # sys.rs -------------------------------------------------------------------
    o = []
    w = o.append
    w("//! Raw FFI declarations of hrl.h (exact C signatures).")
    w("//!")
    w("//! " + note)
    w("#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals, dead_code, clippy::all)]")
    w("")
    w("use std::os::raw::{c_char, c_int, c_uchar, c_uint, c_void};")
    w("")
    for name, t in api.aliases.items():
        w("pub type %s = %s;" % (name, rs_type(api, t)))
    w("")
    for c in api.constants:
        o.extend(rs_doc(c.doc, ""))
        if c.kind == "string":
            w('pub const %s: &str = "%s";' % (c.name, c.value))
        elif c.kind == "float":
            w("pub const %s: f32 = %r;" % (c.name, c.value))
        elif c.kind == "uint":
            w("pub const %s: %s = %d;" % (c.name, c.ctype if c.ctype in api.aliases else "u32", c.value))
        else:
            w("pub const %s: i32 = %d;" % (c.name, c.value))
    w("")
    for e in api.enums:
        o.extend(rs_doc(e.doc, ""))
        w("#[repr(transparent)]")
        w("#[derive(Copy, Clone, Debug, Default, PartialEq, Eq, Hash)]")
        w("pub struct %s(pub c_int);" % e.name)
        w("impl %s {" % e.name)
        for n, v, d in e.values:
            o.extend(rs_doc(d, "    "))
            w("    pub const %s: Self = Self(%d);" % (n, v))
        w("}")
        for n, v, d in e.values:
            w("pub const %s: %s = %s::%s;" % (n, e.name, e.name, n))
        w("")
    for s in api.structs:
        o.extend(rs_doc(s.doc, ""))
        if s.opaque:
            w("#[repr(C)]")
            w("pub struct %s {" % s.name)
            w("    _private: [u8; 0],")
            w("}")
            w("")
            continue
        w("#[repr(C)]")
        w("#[derive(Copy, Clone, Debug)]")
        w("pub struct %s {" % s.name)
        for f in s.fields:
            t = rs_type(api, f.type)
            if f.array:
                t = "[%s; %s]" % (t, consts.get(f.array, f.array))
            w("    pub %s: %s," % ("r#" + f.name if f.name in _KEYWORDS else f.name, t))
        w("}")
        w("impl Default for %s {" % s.name)
        w("    fn default() -> Self {")
        w("        // SAFETY: plain C data, all-zero is a valid value")
        w("        unsafe { std::mem::zeroed() }")
        w("    }")
        w("}")
        w("")
    for c in api.callbacks:
        o.extend(rs_doc(c.doc, ""))
        params = ", ".join("%s: %s" % (rs_name(p.name), rs_type(api, p.type)) for p in c.params)
        ret = "" if c.ret.is_void() else " -> " + rs_type(api, c.ret)
        w("pub type %s = Option<unsafe extern \"C\" fn(%s)%s>;" % (c.name, params, ret))
    w("")
    w("extern \"C\" {")
    for f in api.functions:
        o.extend(rs_doc(f.doc, "    "))
        params = ["%s: %s" % (rs_name(p.name), rs_type(api, p.type)) for p in f.params]
        if f.variadic:
            params.append("...")
        ret = "" if f.ret.is_void() else " -> " + rs_type(api, f.ret)
        w("    pub fn %s(%s)%s;" % (f.name, ", ".join(params), ret))
    w("}")
    files["src/sys.rs"] = "\n".join(o) + "\n"

    # lib.rs -------------------------------------------------------------------
    o = []
    w = o.append
    w("//! Rust bindings of the Horizon Rendering Library.")
    w("//!")
    w("//! " + note)
    w("//!")
    w("//! * [`sys`] : raw FFI, exact C signatures (`HRL_CreateScene`, ...).")
    w("//! * root : the same functions in snake case (`create_scene`) taking Rust")
    w("//!   types : `&str`, slices, tuples for output parameters. Functions")
    w("//!   that take raw pointers or C callbacks stay `unsafe`.")
    w("//! * types, enums and constants are re-exported from [`sys`].")
    w("#![allow(non_camel_case_types, non_upper_case_globals, clippy::too_many_arguments, clippy::missing_safety_doc, clippy::let_unit_value, clippy::let_and_return, clippy::unnecessary_cast, clippy::unnecessary_mut_passed, clippy::manual_c_str_literals)]")
    w("")
    w("pub mod sys;")
    w("")
    w("pub use sys::*;")
    w("")
    w("use std::ffi::{CStr, CString};")
    w("use std::os::raw::{c_char, c_int, c_uint, c_void};")
    w("")
    w("fn cstr(s: &str) -> CString {")
    w("    CString::new(s).expect(\"HRL: string contains a NUL byte\")")
    w("}")
    w("")
    w("unsafe fn from_cstr(p: *const c_char) -> Option<String> {")
    w("    if p.is_null() {")
    w("        None")
    w("    } else {")
    w("        Some(CStr::from_ptr(p).to_string_lossy().into_owned())")
    w("    }")
    w("}")
    w("")
    for f in api.functions:
        roles = param_roles(api, f)
        sizes = sized_by(roles)
        unsafe = any(r in ("pointer", "callback") for r in roles)
        sig = []
        pre = []
        call = []
        outs = []
        out_types = []
        for i, (p, r) in enumerate(zip(f.params, roles)):
            n = rs_name(p.name)
            raw = rs_type(api, p.type)
            t = api.resolve(p.type)
            if r == "size":
                bufs = sizes.get(i, [])
                call.append("_n%d as %s" % (bufs[0], raw) if bufs else "0")
                continue
            if f.variadic and i == len(f.params) - 1:
                call.append("b\"%s\\0\".as_ptr() as *const c_char")
                continue
            if r == "value":
                sig.append("%s: %s" % (n, raw))
                call.append(n)
            elif r == "string":
                sig.append("%s: &str" % n)
                pre.append("let _s%d = cstr(%s);" % (i, n))
                call.append("_s%d.as_ptr()" % i)
            elif r == "bytes":
                sig.append("%s: &[u8]" % n)
                pre.append("let _n%d = %s.len();" % (i, n))
                call.append("%s.as_ptr() as %s" % (n, raw))
            elif r == "array":
                elem = raw.replace("*const ", "", 1)
                sig.append("%s: &[%s]" % (n, elem))
                pre.append("let _n%d = %s.len();" % (i, n))
                call.append("%s.as_ptr()" % n)
            elif r == "strings":
                sig.append("%s: &[&str]" % n)
                pre.append("let _c%d: Vec<CString> = %s.iter().map(|s| cstr(s)).collect();" % (i, n))
                pre.append("let _p%d: Vec<*const c_char> = _c%d.iter().map(|s| s.as_ptr()).collect();" % (i, i))
                pre.append("let _n%d = _p%d.len();" % (i, i))
                call.append("_p%d.as_ptr() as %s" % (i, raw))
            elif r == "out":
                elem = raw.replace("*mut ", "", 1)
                pre.append("let mut _o%d: %s = Default::default();" % (i, elem))
                call.append("&mut _o%d" % i)
                outs.append("_o%d" % i)
                out_types.append(elem)
            elif r == "out_matrix":
                pre.append("let mut _o%d = [0f32; 16];" % i)
                call.append("_o%d.as_mut_ptr()" % i)
                outs.append("_o%d" % i)
                out_types.append("[f32; 16]")
            elif r == "out_string":
                pre.append("let mut _o%d: *const c_char = std::ptr::null();" % i)
                call.append("&mut _o%d" % i)
                outs.append("from_cstr(_o%d)" % i)
                out_types.append("Option<String>")
            elif r == "out_struct":
                elem = raw.replace("*mut ", "", 1)
                pre.append("let mut _o%d = %s::default();" % (i, elem))
                call.append("&mut _o%d" % i)
                outs.append("_o%d" % i)
                out_types.append(elem)
            else:  # pointer / callback
                sig.append("%s: %s" % (n, raw))
                call.append(n)
        if f.variadic:
            sig.append("text: &str")
            pre.append("let _text = cstr(text);")
            call.append("_text.as_ptr()")
        rt = api.resolve(f.ret)
        ret_types = []
        ret_expr = []
        if not f.ret.is_void():
            if rt.base == "char" and rt.pointers == 1:
                ret_types.append("Option<String>")
                ret_expr.append("from_cstr(_r)")
            else:
                ret_types.append(rs_type(api, f.ret))
                ret_expr.append("_r")
        ret_types += out_types
        ret_expr += outs
        ret = ""
        if len(ret_types) == 1:
            ret = " -> " + ret_types[0]
        elif ret_types:
            ret = " -> (" + ", ".join(ret_types) + ")"
        name = snake(f.name)
        if name in _KEYWORDS:
            name += "_"
        o.extend(rs_doc(f.doc, ""))
        if unsafe:
            o.append("///")
            o.append("/// # Safety")
            o.append("/// Pointers and callbacks are passed to C as they are.")
        w("pub %sfn %s(%s)%s {" % ("unsafe " if unsafe else "", name, ", ".join(sig), ret))
        for l in pre:
            w("    " + l)
        body = "sys::%s(%s)" % (f.name, ", ".join(call))
        if unsafe:
            w("    let _r = %s;" % body)
        else:
            w("    // SAFETY: arguments are valid Rust values converted for C")
            w("    let _r = unsafe { %s };" % body)
        if any("from_cstr" in e for e in ret_expr):
            ret_expr = [("unsafe { %s }" % e) if (e.startswith("from_cstr") and not unsafe) else e for e in ret_expr]
        if not ret_types:
            w("    let _ = _r;")
        elif len(ret_expr) == 1:
            w("    " + ret_expr[0])
        else:
            w("    (" + ", ".join(ret_expr) + ")")
        w("}")
        w("")
    files["src/lib.rs"] = "\n".join(o) + "\n"
    files["Cargo.toml"] = CARGO.format(version=api.version)
    files["build.rs"] = BUILD_RS
    return files


CARGO = """# Generated by bindings/generate.py.
[package]
name = "hrl"
version = "{version}.0"
edition = "2021"
description = "Rust bindings of the Horizon Rendering Library (generated)"
license = "Apache-2.0"
links = "hrldll"
build = "build.rs"

[lib]
path = "src/lib.rs"
"""

BUILD_RS = """// Generated by bindings/generate.py.
// HRL_LIB_DIR  : folder containing the HRL library (hrldll.lib / libhrldll.so ...)
// HRL_LIB_NAME : library name without prefix/extension (default "hrldll")
fn main() {
    println!("cargo:rerun-if-env-changed=HRL_LIB_DIR");
    println!("cargo:rerun-if-env-changed=HRL_LIB_NAME");
    if let Ok(dir) = std::env::var("HRL_LIB_DIR") {
        println!("cargo:rustc-link-search=native={}", dir);
    }
    let name = std::env::var("HRL_LIB_NAME").unwrap_or_else(|_| "hrldll".to_string());
    println!("cargo:rustc-link-lib=dylib={}", name);
}
"""
