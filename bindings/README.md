# HRL language bindings

All the bindings are **generated** from the public C headers (`src/hrl.h`, `src/hrl_gl.h`, `src/hrl_vulkan.h`) by `generate.py`, which needs only Python 3.8+ (no SWIG, no libclang). When the API changes:

```
python bindings/generate.py              # every language
python bindings/generate.py python rust  # some of them
generate-bindings.bat                    # Windows (repository root)
```

| Folder | Language | Technique | Needs compiling |
|---|---|---|---|
| `python/` | Python 3.8+ | `ctypes` package `hrl` | no |
| `csharp/` | C# / .NET 6+ | P/Invoke (`HRL.Native` + `HRL.Hrl`) | `dotnet build` |
| `rust/` | Rust 2021 | crate `hrl` (`hrl::sys` raw FFI + safe wrappers) | `cargo build` |
| `go/` | Go 1.21+ | cgo package `hrl` | `go build` (C compiler) |
| `lua/` | LuaJIT 2.1 | FFI module `hrl.lua` | no |
| `c_check/` | C11 | ABI check of the generator | — |

`hrl_api.json` is the intermediate description of the API (functions, parameters, enums, structs, callbacks, constants, documentation); other generators can start from it.

Every binding loads the HRL shared library: `hrldll.dll` (Windows), `libhrldll.so` (Linux), `libhrldll.dylib` (macOS). Put it next to the program or set the path as described below.

## Common conventions

The generator deduces the role of each C parameter and every binding converts it the same way:

| C | Binding side |
|---|---|
| `const char*` text | native string (UTF-8) |
| `const char* data, size_t size` / `const void*, size_t` | bytes (`bytes`, `&[u8]`, `ReadOnlySpan<byte>`, `[]byte`, Lua string) — the size is passed automatically |
| `const T* items, size_t count` (also several parallel arrays sharing one count) | list / slice / span — counts passed automatically |
| `T* out` (scalar, enum, struct) | returned value (tuple, `out` parameter, multiple values) |
| `float* aa` (matrices) | 16 floats |
| `const char** detail` (`HRL_GetLastError`) | returned string |
| callbacks | native functions, kept alive by the binding while registered |
| `HRL_AddScreenMessage(..., format, ...)` | text only (no printf formatting); C# uses `HRL_AddScreenMessageText` |

Enums and constants (`HRL_INVALID_ID`, shader IDs, texture slot names...) are exported with their C names.

## Python

```python
import hrl                                   # bindings/python on PYTHONPATH, or: pip install bindings/python
hrl.HRL_Init(hrl.HRL_VULKAN)
hrl.HRL_InitContext(1280, 720, None)
scene = hrl.HRL_CreateScene(0)
mesh = hrl.HRL_CreateMesh3D(scene, vertices, indices)   # list of hrl.HRL_Vertex3D, list of int
code, detail, severity = hrl.HRL_GetLastError()
hrl.HRL_RegisterErrorCallback(lambda code, sev, detail: print(code, detail))
```

Functions keep their C names; `hrl.raw.HRL_xxx` are the plain ctypes functions. Enums are `IntEnum` classes (`hrl.HRL_ELightType.HRL_POINT_LIGHT`) whose values are also module constants. Library lookup: `HRL_LIBRARY` environment variable (full path), the package folder, the current folder, then the system. Example: `python/examples/cube.py`. With GLFW (`pip install glfw`), pass `glfw.get_proc_address` as the OpenGL loader:

```python
loader = ctypes.cast(glfw.get_proc_address, ctypes.c_void_p)  # or a CFUNCTYPE wrapping it
```

## C#

Add `csharp/HRL.csproj` to the solution (or copy `HRL.cs`, with `AllowUnsafeBlocks`) and copy `hrldll.dll` next to the executable.

```csharp
using HRL;
Hrl.Init(HRL_E_APIs.HRL_VULKAN);
Hrl.InitContext(1280, 720, null);
uint scene = Hrl.CreateScene(0);
uint mesh = Hrl.CreateMesh3D(scene, vertices, indices);    // arrays or spans
var err = Hrl.GetLastError(out string detail, out var severity);
```

`Hrl` exposes the functions without the `HRL_` prefix with .NET types; `Native` holds the exact `DllImport` declarations. Keep a reference to the delegates you pass (the binding also keeps the last one registered). Example: `csharp/examples` (`dotnet run --project csharp/examples -- out.png`).

## Rust

```toml
[dependencies]
hrl = { path = "path/to/hrl/bindings/rust" }
```

```rust
hrl::init(hrl::HRL_VULKAN);
unsafe { hrl::init_context(1280, 720, std::ptr::null_mut()) };
let scene = hrl::create_scene(0);
let mesh = hrl::create_mesh3d(scene, &vertices, &indices);
let (code, detail, severity) = hrl::get_last_error();
```

Linking: `HRL_LIB_DIR` = folder of `hrldll.lib`/`libhrldll.so`, `HRL_LIB_NAME` if the library is not called `hrldll`. Functions taking raw pointers or C callbacks are `unsafe`; `hrl::sys` is the raw FFI. Example: `cargo run --example cube -- out.png texture.png`.

## Go

```go
import "github.com/oscar-soirey/hrl/bindings/go/hrl"  // or a replace directive to the local folder

hrl.Init(hrl.HRL_VULKAN)
hrl.InitContext(1280, 720, nil)
scene := hrl.CreateScene(0)
mesh := hrl.CreateMesh3D(scene, vertices, indices)
hrl.RegisterErrorCallback(func(code hrl.EError, sev hrl.ESeverity, detail string) { ... })
```

`CGO_LDFLAGS=-L<library folder>`; for another library name: `CGO_LDFLAGS="-L... -lhrl" go build -tags hrl_custom_lib`. Types lose the `HRL_` prefix (`hrl.Vertex3D`, `hrl.ELightType`); struct layouts are checked against C at compile time. HRL must be called from one OS thread: call `runtime.LockOSThread()` in `main`'s `init`. Example: `go run ./examples/cube out.png`.

## LuaJIT

```lua
local hrl = require("hrl")     -- HRL_LIBRARY = full path of the library if needed
hrl.Init(hrl.HRL_VULKAN)
hrl.InitContext(1280, 720, nil)
local scene = hrl.CreateScene(0)
local code, detail, severity = hrl.GetLastError()
```

Functions without the `HRL_` prefix; `hrl.C` is the raw FFI namespace, `hrl.ffi` the ffi module. Example: `luajit lua/examples/cube.lua out.png`.

## Tests done

The generator was run on HRL 0.8 (456 functions, 29 enums, 11 structs, 6 callbacks, 41 constants). `c_check/hrl_abi_check.c` compiles against the headers with every pointer type checked (all signatures, enum values, constants, struct fields). The Python, Rust, Go and LuaJIT examples render the same cube through the Vulkan backend (offscreen) on Linux, with the error callback, strings, byte buffers, struct arrays and out parameters. The C# code was checked for syntax only (no .NET SDK was available).

## Old SWIG bindings

`hrl.i`, `hrl_python.i`, `generate-python.bat` and the content of `python/` produced by SWIG (`hrl.py`, `hrl_wrap.cxx`, `_hrl.pyd`, `setup.py`, `build/`) are replaced by this generator and can be deleted.
