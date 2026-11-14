# Programmable render pipeline (Vulkan)

The Vulkan backend lets an application insert its own GPU passes in the frame
of a viewport: compute passes that read and write render targets, fullscreen
passes that draw into targets or into the HDR image of the scene, and a
participating media volume that HRL applies to every surface. It is the base
of custom renderers such as `hrl::heaven::HeavenStyle` (`src/extras`).

Everything here needs the Vulkan backend built with glslang
(`HRL_VULKAN_RUNTIME_SHADERS`, ON by default): pass shaders are compiled at run
time, like the custom surface shaders. Other backends report
`HRL_INVALID_BACKEND_OPERATION`.

## Frame of a viewport

```
shadow maps
  HRL_STAGE_BEFORE_OPAQUE      lights and shadows ready, no depth: participating media, light volumes
opaque surfaces, sky           (the surfaces read the media volume)
  HRL_STAGE_AFTER_OPAQUE       depth final: volumes marched against it, composited into the scene
translucent surfaces, particles
  HRL_STAGE_AFTER_TRANSLUCENT
anti-aliasing (TAA), screen space effects, post-processes (HRL_CreatePostProcess)
  HRL_STAGE_BEFORE_TONEMAP     final HDR image: lens, grading
bloom, tone mapping, built-in grading (vignette, grain...)
```

When a viewport has `HRL_STAGE_AFTER_OPAQUE` passes, its translucent surfaces
are drawn in a pass of their own after them, so whatever those passes
composite (smoke, clouds) lies under the glass and the particles.

## Render targets

```c
HRL_id t = HRL_CreateViewportRenderTarget(scene, HRL_RENDER_TARGET_RGBA16F, 0.5f, 1);  // half resolution 2D
HRL_id v = HRL_CreateViewportRenderTarget(scene, HRL_RENDER_TARGET_RGBA16F, 1.f / 8.f, 96); // froxels (3D)
HRL_id f = HRL_CreateRenderTarget(scene, HRL_RENDER_TARGET_R16F, 64, 64, 1);           // fixed size
HRL_SetRenderTargetHistory(t, HRL_TRUE);   // keeps the previous frame (temporal accumulation)
HRL_ResetRenderTargetHistory(t);           // camera cut: no valid history next frame
HRL_SetRenderTargetLinearFilter(t, HRL_FALSE);
```

A target belongs to a scene and is **instanced per viewport**: every viewport
running a pass that uses it has its own images (and its own history). Images
are created when a frame first needs them, cleared to zero, recreated when the
viewport changes size, and released a few frames after their last use.

## Render passes

```c
HRL_id shader = HRL_CreateComputeShader(source, size);      // or HRL_CreateFullscreenShader
HRL_id material = HRL_CreateMaterial(shader);                // parameters = the shader's uniforms
HRL_id pass = HRL_CreateRenderPass(viewport, HRL_STAGE_AFTER_OPAQUE, 0, material);
HRL_SetRenderPassTarget(pass, "Out", t, HRL_PASS_WRITE);
HRL_SetRenderPassTarget(pass, "History", t, HRL_PASS_READ_PREVIOUS);
HRL_SetRenderPassDispatchTarget(pass, t);                    // one invocation per texel of t
HRL_SetRenderPassBuffer(pass, "Volumes", data, bytes);       // HRL_BUFFER(Volumes)
HRL_MaterialSetFloat(material, "Strength", 2.f);
```

Passes of a stage run by priority (lowest first), then in creation order. A
compute pass dispatches one invocation per pixel of its viewport unless
`HRL_SetRenderPassDispatchTarget` / `HRL_SetRenderPassDispatchSize` say
otherwise; `HRL_SetRenderPassDispatchDivisor` divides that size (for example
`(1, 1, depth)`: one invocation per column of a 3D target). A fullscreen pass
draws into `HRL_SetRenderPassOutput` outputs (up to 4 targets of one size, or
`HRL_SCENE_COLOR_TARGET`) with a blend mode (`NONE`, `ALPHA`, `ADDITIVE`,
`PREMULTIPLIED`: target * a + rgb).

A pass whose resources are missing or mismatched (2D / 3D, format, access) is
skipped and reported once.

## Shader contract

Pass shaders are GLSL 4.60 bodies; HRL adds the `#version` line, the view
(`uView`), the lights and their shadows (`lighting.glsl`), the participating
media (`media.glsl`) and the pass contract (`src/shaders/vulkan/pass.glsl`).

```glsl
layout(local_size_x = 8, local_size_y = 8) in;   // compute passes
HRL_IMAGE2D(rgba16f, Out);        // storage image, bound with HRL_PASS_WRITE (compute only)
HRL_TEXTURE2D(History);           // sampled, HRL_PASS_READ or HRL_PASS_READ_PREVIOUS
HRL_IMAGE3D(rgba16f, Volume);  HRL_TEXTURE3D(VolumeIn);
HRL_BUFFER(Volumes) { vec4 count; MyVolume volumes[]; };
uniform float Strength = 1.0;     // material parameter (float, int, bool, vecN, ivecN, sampler2D, mat3/4, arrays)
uniform sampler2D Noise;          // an HRL texture (HRL_MaterialSetTexture)

void main()
{
	ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(p, imageSize(Out)))) return;
	vec2 uv = (vec2(p) + 0.5) / vec2(imageSize(Out));      // viewport UV, origin top-left
	float depth = HRL_SceneDepth(uv);                         // reversed-Z, 0 = sky
	vec3 world = HRL_WorldPosition(uv, depth);
	vec3 light = vec3(0.0);
	for (uint i = 0u; i < HRL_LightCount(); ++i)
	{
		vec3 L;
		light += HRL_LightRadiance(i, world, L);                // with the shadow of the light
	}
	imageStore(Out, p, vec4(light * Strength, 1.0));
}
```

Fullscreen passes get `vUV` (viewport UV, origin top-left) and declare their
outputs: `layout(location = 0) out vec4 color;`. The material parameter
`uniform` declarations follow the rules of the custom surface shaders. Names of
the HRL GLSL code (`Light`, `Material`, `uView`, `lights`...) are taken.

Built-in resources, bound when the shader uses them:

| Name | What |
|-|-|
| `hrlPass.viewportRect` | rectangle of the viewport in the scene images (pixels) |
| `hrlPass.sceneSize`, `.dispatchSize` | sizes |
| `hrlPass.frame` | x frame number, y time (s), z time since the last frame, w 1 when every history read is valid |
| `hrlPass.jitter`, `.viewProjNoJitter`, `.previousViewProj`, `.previousCamera`, `.previousForward` | temporal reprojection |
| `hrl_Depth` | depth of the scene (after the opaque surfaces) |
| `hrl_SceneColor` | copy of the HDR scene made before the pass |
| `hrl_SceneImage` | the HDR scene itself, read / write (compute) |

Helpers (`pass.glsl`, `media.glsl`): `HRL_SceneUV`, `HRL_ScenePixel`,
`HRL_SceneDepth`, `HRL_IsSky`, `HRL_WorldPosition`, `HRL_ViewRay`,
`HRL_ViewDepth`, `HRL_PreviousUV`, `HRL_PreviousFroxelUVW`, `HRL_LightCount`,
`HRL_LightRadiance`, `HRL_MainLight`, `HRL_AmbientLight`, `HRL_PhaseHG`,
`HRL_FrameNoise`, `HRL_Hash12/13`, `HRL_FroxelUVW`, `HRL_FroxelWorld`,
`HRL_SampleMedia`, `HRL_ApplyMedia`, `HRL_MediaVisibility`, `HRL_CameraForward`.

## Participating media

```c
HRL_SetViewportMediaVolume(viewport, integrated, visibility, 110.f, 2.f);
```

`integrated` is a 3D target in froxels over the view: rgb = light scattered
towards the camera between the camera and the froxel, a = transmittance.
HRL applies it to every lit surface (opaque and translucent) and to the sky:
`color * a + rgb`. `visibility` (optional) is a 3D target of the same mapping
whose red channel scales the first directional light on the surfaces (the
shadows of volumes). Froxel mapping: x, y over the viewport (top-left origin),
z = (view depth / max distance) ^ (1 / exponent).

The application writes these volumes with its own passes at
`HRL_STAGE_BEFORE_OPAQUE` (see the Heaven style for a complete froxel
implementation: injection with every light and its shadows, temporal
reprojection, energy conserving integration).

## Custom post-processes: automatic uniforms

The OpenGL style post-processes of `HRL_CreatePostProcess` now also receive on
Vulkan, when they declare them: `uView`, `uProjection`, `uViewProjection`,
`uInvView`, `uInvProjection`, `uInvViewProjection`, `uCameraPos`,
`uCameraForward`, `uNearFar`, `uViewportOrigin`, `uViewportSize`,
`uSunDirection`, `uSunColor`, `uSunScreen`.

## C++: CustomShaderRenderer and the Heaven style

`src/extras` holds header-only C++ built on the C API:

- `custom_shader_renderer.hpp`, `hrl::CustomShaderRenderer`: a custom renderer
  for one viewport. Named targets and passes, surface shaders and their
  materials, code shared by every shader (`SetSharedCode`), parameters shared by
  every material (`SetGlobal*`), the media volume and the built-in grading.
  Everything it made is released by its destructor.
- `heaven_style.hpp`, `hrl::heaven::HeavenStyle`: a dark, heavy, smoke filled
  look built on it: froxel participating media lit by every light with its
  shadows, smoke volumes ray marched at half resolution (self shadowed,
  multiple scattering, emission, temporal accumulation, depth aware
  upsampling), sun shadows of the smoke on the surfaces, a lens (heat haze,
  soft corners, fringes, halation) and a warm grade, smooth back lit surface
  materials that can tear into smoke, rising embers.

```cpp
#include <extras/heaven_style.hpp>

hrl::heaven::HeavenStyle heaven(scene, viewport);
heaven.SetGlow(0.f, 4.5f, -44.f, 9.f, 4.6f, 2.f);
hrl::heaven::Smoke smoke;
smoke.center = {0.f, 1.f, -20.f};
smoke.halfExtents = {12.f, 1.5f, 8.f};
heaven.AddSmoke(smoke);
HRL_SetMeshMaterial(tree, heaven.CreateFormMaterial("tree", hrl::heaven::Form{}));
// every frame, before HRL_EndFrame:
heaven.Update();
```

The `hrl_vulkan_heaven` example is a complete scene; `--set name=value`
changes any setting without recompiling, `--debug noise|media|smoke` shows an
input of the style.

Cost on the GPU (1920 x 1080, defaults): froxels 240 x 135 x 96, smoke at
960 x 540 with 40 steps (5 shadow steps per light), lens at full resolution.
`Settings::froxelScale`, `froxelSlices`, `smokeScale`, `smokeSteps` and
`smokeShadowSteps` trade quality for time.
