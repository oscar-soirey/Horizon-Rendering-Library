# HRL lighting example

## Sky light

```cpp
HRL_id sky = HRL_CreateLight(scene, HRL_SKY_LIGHT);
HRL_SetLightColor(sky, 0.8f, 0.9f, 1.0f);
HRL_SetLightIntensity(sky, 0.15f);
```

A sky light is ambient and does not require any shadow-map settings.

## Screen-space ambient occlusion

SSAO is disabled by default. Enable it only if the scene needs it:

```cpp
HRL_SetAmbientOcclusionEnabled(scene, HRL_TRUE);
HRL_SetAmbientOcclusionStrength(scene, 0.65f);
HRL_SetAmbientOcclusionRadius(scene, 1.0f);
HRL_SetAmbientOcclusionBias(scene, 0.03f);
HRL_SetAmbientOcclusionPower(scene, 1.4f);
```

## Two-sided materials

Meshes are one-sided by default. To render both faces:

```cpp
HRL_MaterialSetBool(
    material,
    HRL_MATERIAL_PARAM_TWO_SIDED,
    HRL_TRUE
);
```

No depth, culling, or shadow settings need to be managed by the application.
