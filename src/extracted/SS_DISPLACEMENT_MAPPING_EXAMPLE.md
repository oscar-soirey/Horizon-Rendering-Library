# Screen Space Displacement Mapping

The built-in OpenGL 3D and skinned materials support a screen-space displacement texture. HRL automatically renders these materials in an internal second pass after the scene color is available; application code never manages the framebuffer or depth state.

```cpp
HRL_MaterialSetTexture(
    material,
    HRL_MATERIAL_TEXTURE_SS_DISPLACEMENT_MAPPING,
    displacementTexture
);

HRL_MaterialSetFloat(material, HRL_MATERIAL_PARAM_SS_DISPLACEMENT_STRENGTH, 18.0f);
HRL_MaterialSetFloat(material, HRL_MATERIAL_PARAM_SS_DISPLACEMENT_SCALE, 1.0f);
HRL_MaterialSetFloat(material, HRL_MATERIAL_PARAM_SS_DISPLACEMENT_OPACITY, 1.0f);

HRL_id mesh = HRL_CreateMesh3D(scene, vertices, vertexCount, indices, indexCount);
HRL_SetMeshMaterial(mesh, material);
```

The RG channels form a 2D screen-space vector field: 0.5/0.5 is neutral. Strength is measured in screen pixels.
