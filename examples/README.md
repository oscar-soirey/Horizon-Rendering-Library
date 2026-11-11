# HRL examples

Built with the library:

```
cmake -S . -B build -DHRL_BUILD_EXAMPLES=ON
cmake --build build --config Release -j
```

The executables are written next to the HRL library, in `build/bin`
(`build/bin/Release` with Visual Studio), and run from there.

| Target | Backend | Content |
|-|-|-|
| `hrl_vulkan_pbr` | Vulkan | PBR materials, cascaded shadows, image based lighting, ray traced shadows (`--rt`). No external resource. |
| `hrl_editor` | Vulkan | Small scene editor with a [Raven](https://github.com/oscar-soirey/Raven) interface: add, select, move, duplicate and delete objects, edit their material, lighting, save / load. See below. |
| `hrl_gl_showcase` | OpenGL 3.3 | Full feature tour (FBX, landscape, VFX, widgets, GI) |
| `hrl_gl_minimal` | OpenGL 3.3 | Smaller 3D scene |
| `hrl_gl_vfx` | OpenGL 3.3 | Particle systems |
| `hrl_gl_voxel` | OpenGL 3.3 | 2D voxel world |
| `hrl_gl_widgets` | OpenGL 3.3 | UI widgets |

## Scene editor (`hrl_editor`)

Raven is downloaded at configure time (pinned commit, `HRL_RAVEN_GIT_TAG`); pass
`-DHRL_RAVEN_DIR=<path>` to use a local checkout instead. Windows and Linux (X11 / Wayland).

The Raven `<Viewport>` owns a native child window; HRL creates its Vulkan surface and
swapchain on it (`examples/editor/native_surface.cpp`) and renders there with its own
device. The editor only uses the public API of HRL (`hrl.h`, `hrl_vulkan.h`).

| Input (3D view) | Action |
|---|---|
| left click / left drag | select / move on the horizontal plane (Shift: up and down) |
| right button + mouse, W A S D Q E | look around, fly (Shift: faster) |
| wheel, F, Delete | forward / backward, look at the selection, remove it |
| Ctrl+D, Ctrl+S, Ctrl+O | duplicate, save, load (`hrl_editor_scene.txt` in the working directory) |

Automated run: `HRL_EDITOR_FRAMES=60 HRL_EDITOR_CAPTURE=out hrl_editor` renders 60 frames,
writes `out_scene.png` and `out_ui.ppm`, checks the picking and exits.

## Resources

The OpenGL examples read their textures, models and fonts from
`examples/assets` (the program changes its working directory to it at start).
This folder is not versioned (about 150 MB); it holds:

```
skeletal.fbx  model.fbx  FiraCode-Regular.ttf  FiraCode-Bold.ttf  skydome.jpg
heightmap700.jpg  MorningSkyHDRI011B_12K_TONEMAPPED.jpg
wall/Rock064_4K-JPG_*.jpg  floor/Tiles086_1K-JPG_*.jpg
```
