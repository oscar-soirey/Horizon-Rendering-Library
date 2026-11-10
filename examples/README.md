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
| `hrl_gl_showcase` | OpenGL 3.3 | Full feature tour (FBX, landscape, VFX, widgets, GI) |
| `hrl_gl_minimal` | OpenGL 3.3 | Smaller 3D scene |
| `hrl_gl_vfx` | OpenGL 3.3 | Particle systems |
| `hrl_gl_voxel` | OpenGL 3.3 | 2D voxel world |
| `hrl_gl_widgets` | OpenGL 3.3 | UI widgets |

## Resources

The OpenGL examples read their textures, models and fonts from
`examples/assets` (the program changes its working directory to it at start).
This folder is not versioned (about 150 MB); it holds:

```
skeletal.fbx  model.fbx  FiraCode-Regular.ttf  FiraCode-Bold.ttf  skydome.jpg
heightmap700.jpg  MorningSkyHDRI011B_12K_TONEMAPPED.jpg
wall/Rock064_4K-JPG_*.jpg  floor/Tiles086_1K-JPG_*.jpg
```
