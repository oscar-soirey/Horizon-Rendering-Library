#!/usr/bin/env python3
"""Generates the HRL language bindings from the public C headers.

    python bindings/generate.py              # every language
    python bindings/generate.py python rust  # some of them

Only the Python standard library is needed. The C headers are parsed into
bindings/hrl_api.json (intermediate representation, useful for other tools)
and every binding is written in its folder :

    bindings/python   ctypes package "hrl" (no compilation)
    bindings/csharp   P/Invoke (HRL.Native + friendly HRL.Hrl class), .NET 6+
    bindings/rust     crate "hrl" (raw FFI in hrl::sys + safe wrappers)
    bindings/go       cgo package "hrl"
    bindings/lua      LuaJIT FFI module
    bindings/c_check  C program checking the parser against the compiler
"""
import json
import shutil
import subprocess
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from hrlgen import parser, emit_python, emit_csharp, emit_rust, emit_go, emit_lua, emit_ccheck  # noqa: E402

HEADERS = ["hrl.h", "hrl_gl.h", "hrl_vulkan.h"]


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("  wrote", os.path.relpath(path, os.path.dirname(HERE)))


def main(argv):
    src = os.path.join(os.path.dirname(HERE), "src")
    texts = []
    for h in HEADERS:
        with open(os.path.join(src, h), encoding="utf-8") as f:
            texts.append((h, f.read()))
    api = parser.parse(texts)
    print("HRL %s : %d functions, %d enums, %d structs, %d callbacks, %d constants" % (
        api.version, len(api.functions), len(api.enums), len(api.structs), len(api.callbacks), len(api.constants)))
    for name, reason in api.skipped:
        print("  note: %s skipped (%s)" % (name, reason))

    langs = argv or ["python", "csharp", "rust", "go", "lua", "c_check"]
    write(os.path.join(HERE, "hrl_api.json"), json.dumps(api.to_json(), indent=1) + "\n")
    for lang in langs:
        if lang == "python":
            write(os.path.join(HERE, "python", "hrl", "__init__.py"), emit_python.emit(api, HEADERS))
            write(os.path.join(HERE, "python", "hrl", "_loader.py"), emit_python.LOADER)
            write(os.path.join(HERE, "python", "hrl", "py.typed"), "")
            write(os.path.join(HERE, "python", "pyproject.toml"), emit_python.PYPROJECT.format(version=api.version))
        elif lang == "csharp":
            for name, text in emit_csharp.emit(api, HEADERS).items():
                write(os.path.join(HERE, "csharp", name), text)
        elif lang == "rust":
            for name, text in emit_rust.emit(api, HEADERS).items():
                write(os.path.join(HERE, "rust", name), text)
        elif lang == "go":
            for name, text in emit_go.emit(api, HEADERS).items():
                write(os.path.join(HERE, "go", name), text)
            if shutil.which("gofmt"):  # optional : canonical Go formatting
                subprocess.call(["gofmt", "-w", os.path.join(HERE, "go", "hrl")])
        elif lang == "lua":
            for name, text in emit_lua.emit(api, HEADERS).items():
                write(os.path.join(HERE, "lua", name), text)
        elif lang == "c_check":
            write(os.path.join(HERE, "c_check", "hrl_abi_check.c"), emit_ccheck.emit(api))
        else:
            print("unknown language:", lang)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
