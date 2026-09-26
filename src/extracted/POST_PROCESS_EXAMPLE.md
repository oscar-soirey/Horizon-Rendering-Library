# Built-in post-process shader example

```cpp
HRL_id postMaterial = HRL_CreateMaterial(HRL_DEFAULT_POST_PROCESS_SHADER);

HRL_MaterialSetFloat(postMaterial, "exposure", 0.5f);
HRL_MaterialSetFloat(postMaterial, "hueShift", 8.0f);
HRL_MaterialSetFloat(postMaterial, "sharpenStrength", 0.25f);
HRL_MaterialSetFloat(postMaterial, "chromaticAberration", 1.5f);

HRL_MaterialSetFloat(postMaterial, "vignetteStrength", 0.35f);
HRL_MaterialSetFloat(postMaterial, "vignetteRadius", 0.72f);
HRL_MaterialSetFloat(postMaterial, "vignetteSoftness", 0.28f);
HRL_MaterialSetVec3(postMaterial, "vignetteColor", 0.0f, 0.0f, 0.0f);

HRL_MaterialSetFloat(postMaterial, "filmGrainStrength", 0.08f);
HRL_MaterialSetFloat(postMaterial, "filmGrainScale", 1.0f);

HRL_id post = HRL_CreatePostProcess(viewport, postMaterial, 0);
```

Existing controls remain available: `brightness`, `contrast`, `saturation`, `gamma`, `tintColor`, `invertColor`, and `bloomStrength`.


## Built-in material texture semantic

The built-in OpenGL 3D material also exposes the named texture semantic `SS_DISPLACEMENT_MAPPING`.
Use the existing material texture API:

```cpp
HRL_MaterialSetTexture(material, HRL_MATERIAL_TEXTURE_SS_DISPLACEMENT_MAPPING, texture);
```

This reserves the texture slot for screen-space displacement mapping while keeping the existing material API unchanged.
