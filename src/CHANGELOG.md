# Changelog

## 2026-09-26 — SSGI pipeline rewrite

### Global illumination
- Reworked the OpenGL 3.3 SSGI frame graph around an explicit resolved-scene-color copy.
- Removed temporal accumulation from the base implementation so the GI signal is deterministic and directly debuggable.
- Replaced the previous sparse view-space ray marcher with a 24-sample screen-space radiance gather using:
  - screen-space radius in pixels;
  - reconstructed view-space positions from depth;
  - world-space G-buffer normals transformed into view space;
  - receiver/source orientation weighting;
  - depth-aware proximity weighting;
  - HDR radiance clamping.
- Added horizontal + vertical bilateral denoising.
- Composite now samples an immutable scene-color copy and writes directly to the real scene color attachment, avoiding texture feedback.
- Restored all five scene draw buffers after the GI composite so subsequent viewport rendering keeps updating the complete G-buffer.
- Preserved GI results in the MSAA render target so a later MSAA resolve cannot erase the previous viewport's GI result.
- Kept GI scene textures synchronized on window resize.

### Validation
- SSGI, denoise, and composite GLSL programs were compiled and linked in a headless Mesa OpenGL 4.5 context using GLSL `#version 330 core`.
- A synthetic offscreen scene was rendered through the raw SSGI and composite stages with zero OpenGL errors; the test produced measurable indirect radiance and a changed final color on the receiving surface.
- `gi.cpp` was syntax-checked with Clang using generated interface stubs; only pre-existing project warnings were reported.
