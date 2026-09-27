# HRL VFX / Particle API

HRL now exposes a Niagara-like effect hierarchy:

```text
VFX System
├── Emitter
│   ├── Spawn / Bursts
│   ├── Lifetime
│   ├── Initial velocity / rotation
│   ├── Gravity / Drag / Force / Noise
│   ├── Color / Size / Rotation curves
│   ├── Collision
│   └── Billboard / Stretched Billboard / Mesh renderer
└── Emitter ...
```

Systems and emitters use `HRL_id`. Individual particles intentionally do not: particles are simulation data owned internally by the emitter.


### 3D mesh particles

An emitter can render a static HRL 3D mesh once per particle. The mesh is used as a template: its scene position/rotation are ignored, while each particle supplies its own position, rotation and scale. The mesh keeps its normal HRL material unless the emitter has a compatible `HRL_MESH_3D_SHADER` material override.

```cpp
HRL_SetVFXEmitterRenderMode(emitter, HRL_VFX_RENDER_MESH);
HRL_SetVFXEmitterMesh(emitter, mesh);
HRL_SetVFXEmitterMeshScale(emitter, 0.25f, 0.25f, 0.25f);
HRL_SetVFXEmitterMeshRotation(emitter, 0.0f, 90.0f, 0.0f);
```

`HRL_SetVFXEmitterParticleSize()` and the size curve still control the per-particle scale. The mesh-scale setter is the base scale applied to every particle. Static meshes created with `HRL_CreateMesh3D` or imported into an HRL mesh can be used.

## Update model

By default VFX systems are updated automatically from `HRL_BeginFrame()`.
For deterministic/manual control:

```cpp
HRL_SetVFXSystemAutoUpdate(system, HRL_FALSE);
HRL_UpdateVFX(1.0f / 60.0f);
```

## Coordinate spaces

`HRL_VFX_SIMULATION_LOCAL` keeps particles relative to the system transform.
`HRL_VFX_SIMULATION_WORLD` converts their spawn transform into world space, so moving the system later does not move existing particles.

## Rendering

Depth handling is automatic. VFX particles depth-test against opaque scene geometry and never write to the scene depth buffer. Application code does not need to configure depth state.


The OpenGL 3.3 backend supports alpha, additive and multiply blending. Billboards are camera-facing and the stretched mode aligns the long axis with the particle velocity projected onto the camera plane.

Mesh renderers reuse existing HRL mesh GPU buffers and are intentionally not assigned one HRL object ID per particle.

## Curves

Color curves store RGBA keys in normalized particle lifetime `[0..1]`. Float curves store scalar keys and can drive particle size or rotation.

## Collision

Without a collision scene, collision uses a y=0 ground plane. When `HRL_SetVFXCollisionScene()` is enabled, current HRL mesh bounding spheres are used as a lightweight collision approximation.

