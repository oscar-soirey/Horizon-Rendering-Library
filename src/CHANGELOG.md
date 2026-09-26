- SS displacement is now opt-in through `ss_displacement_enabled`; the default is disabled even when a displacement texture is assigned.
- Added per-light shadow strength control via HRL_SetLightShadowStrength() (0 = no darkening, 1 = full shadow).
- Added viewport gizmos independent from HRL scene objects. Gizmos have their own HRL ids, world/local space, translate/rotate/scale modes, per-axis visibility/colors, hover state, screen/world sizing, mouse interaction, transform getters and change callback support.
- Widget sizing is now resize-safe: SetWidgetSize keeps the size captured at layout time in pixels, while positions/anchors remain normalized and responsive.
- Finalized the widget system without removing the existing widget API: Button, Label, Image, Slider, Checkbox and ProgressBar are all creatable.
- Added viewport-normalized anchors, visibility/enabled state, alpha and deterministic z-ordering.
- Added mouse press/release input routing with topmost-widget capture for overlapping controls.
- Implemented button hover/pressed states and callbacks, text sizing, labels, images, interactive sliders, checkboxes and progress bars.
- Preserved the legacy button background texture convention where texture ID 0 selects the default white texture.
### Window/framebuffer resize synchronization
- HRL/OpenGL now updates the default framebuffer viewport immediately on resize.
- All five scene color attachments are resized, including DDGI albedo/normal targets.
- Scene depth and MSAA resources are recreated at the new framebuffer dimensions.
- Resize documentation now explicitly requires framebuffer/drawable dimensions for HiDPI windows.

## Unreleased — DDGI stability / lifetime / OpenGL robustness

- Fixed post-process lifetime management: stable IDs are now used for viewport ownership, duplicate priorities are supported, and deletion removes every dangling viewport reference.
- Fixed camera/viewport lifetime hazards and rejected cross-scene camera assignments; viewports with deleted cameras are skipped safely by the OpenGL renderer.
- Fixed unsupported widget creation returning a null object and implemented widget destruction/viewport ownership cleanup.
- Fixed shutdown cleanup for pointer-owned post-processes, widgets, scene resources, and debug state.
- Fixed shader source handling to use explicit buffer lengths and cleaned up shader objects on compile/link failures.
- Hardened OpenGL texture loading, removed process-global stb_image flipping, cached CPU RGBA data for DDGI material sampling, and stopped generating mipmaps from magnification filtering.
- Replaced per-frame DDGI scene hashing with explicit geometry/lighting revisions and added invalidation for transforms, lights, materials, textures, spotlight cones and shadow bias.
- Added UV-aware DDGI albedo sampling, metallic diffuse weighting, secondary shadow rays for probe direct lighting, explicit probe validity, and temporal probe accumulation.
- Cached triangle centroids for BVH construction and moved normal-matrix inversion out of the static mesh vertex shader.
- Added finite-value validation for static and skeletal vertex attributes and robust TBN/normal fallbacks to prevent NaNs from reaching the renderer.

## Unreleased — DDGI world-space probe volume

- Replaced the OpenGL 3.3 SSGI implementation with a camera-independent DDGI-style irradiance probe volume.
- Added an incremental CPU ray-traced probe update path using the scene's static triangle data and current lights.
- Added cheap iterative multi-bounce feedback by sampling the previous probe irradiance at ray hits.
- Added six trilinear 3D irradiance volumes (+X/-X/+Y/-Y/+Z/-Z) and world-space probe interpolation in the final composite pass.
- Added probe-volume rebuild detection when scene geometry transforms, materials or lights change.
- Kept the final GI application in the resolved scene framebuffer so post-processing receives the indirect-lit image.
- OpenGL 3.3 now reports `HRL_GI_DDGI` as its supported GI method.
- The default scene GI method is now `HRL_GI_DDGI`.
- This first DDGI implementation intentionally targets static 3D meshes; sprites and skeletal meshes are not injected into the probe ray scene yet.


## Unreleased — GI G-buffer integration

- Fixed `GL_33_GI::RenderSSGI()` so the camera far plane is passed explicitly instead of accessing the renderer-private `ctx_`.
- Added five scene color attachments internally: scene color, bloom, picking, GI diffuse albedo, and GI world-space normal.
- Added matching MSAA attachments and resolves for the GI buffers.
- 3D static/skinned materials now write unlit diffuse albedo and final world-space normal into the GI G-buffer after alpha rejection.
- SSGI now consumes the dedicated albedo/normal buffers instead of reconstructing normals from depth or guessing receiver albedo from Scene Color.
- The receiver diffuse term is evaluated from the G-buffer albedo (`albedo / pi`) while source radiance remains sampled from Scene Color.
- The normal G-buffer contains the final normal-map-aware world-space shading normal, so SSGI no longer loses normal-map detail.
- Added OpenGL accessors for the GI albedo and normal buffers.


## SSGI v12

- Replaced the prototype cosine-hemisphere ray march with a horizon-based screen-space indirect diffuse estimator based on the published GTAO/SSRT3 family of techniques.
- Uses two rotated horizon slices and exponentially distributed samples (2 slices × 8 steps × 2 directions).
- Added finite-thickness backface testing, source/receiver cosine terms, and HDR luminance clamping.
- Kept temporal reprojection and bilateral denoising from the previous integration.
- SSGI remains disabled by default.


## Upcoming - SSGI quality improvements
- Added stochastic temporal accumulation and view reprojection for the OpenGL 3.3 SSGI backend.
- Added history-depth validation, neighborhood clipping, and two-pass depth/normal-aware bilateral denoising.
- Kept GI fully disabled unless explicitly selected and preserved the existing skinning, materials, and shadow paths.

- Correction de la reconstruction des matériaux FBX : les valeurs d’opacité synthétisées par ufbx pour les anciens matériaux Phong/Lambert ne sont plus interprétées comme une transparence réelle. Un faux `opacity = 0` pouvait provoquer un `discard` de tous les fragments, laissant le maillage visible en wireframe et dans les passes d’ombre mais invisible en rendu lit.
- Les textures d’opacité/transparence ne sont maintenant liées que lorsqu’elles sont réellement activées dans le matériau FBX.
# HRL — Skeletal mesh correction

## Ajouté

- `HRL_SkeletalMeshData::geometryToWorldMatrix[16]` en fin de structure publique pour transporter le transform géométrique du mesh importé FBX sans perturber l'ordre des champs existants.
- Stockage interne de ce transform dans `HRL_SkeletalMesh::fbx_geometry_to_world_`.

## Modifié

### Import FBX / ufbx

- Le chargement des données skeletal continue d'utiliser `#include <ufbx/ufbx.h>` et `ufbx_load_memory()`.
- Le chargement active l'évaluation du skinning ufbx.
- Les matrices de pose sont maintenant basées sur `ufbx_skin_cluster::geometry_to_world`, qui représente directement `bone_node->node_to_world * geometry_to_bone`.
- Les clusters évalués sont associés par `bone_node->typed_id` afin de ne pas dépendre des pointeurs des nodes de la scène originale.
- Le transform `meshNode->geometry_to_world` est retiré des matrices de skinning pour conserver des matrices locales HRL.
- Le même transform est appliqué une seule fois dans la matrice modèle du skeletal mesh.

### Renderer

- `CalculateModelMatrix()` applique `fbx_geometry_to_world_` uniquement aux skeletal meshes.
- Le transform utilisateur `position / rotation / scale` reste distinct du transform importé FBX.

### Compatibilité

- Aucun symbole existant supprimé.
- Aucun paramètre de fonction existant modifié.
- `hrl_gl.h` inchangé.
- Les anciennes structures `HRL_SkeletalBone` et `HRL_SkeletalAnimation` restent inchangées.
- Le nouveau champ de `HRL_SkeletalMeshData` est placé à la fin pour préserver les initialiseurs agrégés existants.

## Test

Avec l'exemple unique :

```text
examples/main.cpp
```

charger un `skeletal.fbx` à côté de l'exécutable, puis vérifier :

```text
Skeletal mesh: <nombre> bones, <nombre> animations
Skeletal model camera distance: <distance>
```

Le mesh doit maintenant être rendu avec l'ensemble de sa géométrie dans l'espace local HRL et suivre correctement les matrices de skinning évaluées par ufbx.

## Nettoyage

Aucun fichier `fake_*`, mock, repro, `.bak`, fichier d'inspection ou autre fichier de travail n'est inclus.

## Skeletal FBX skinning fix

- Fixed FBX skeletal skin matrices being converted into a mixed coordinate space.
- `ufbx_skin_cluster::geometry_to_world` is now kept as the complete skin matrix.
- The imported mesh node's `geometry_to_world` remains a separate model transform.
- This matches the transform convention used by the ufbx reference viewer and prevents skinned geometry from being displaced/collapsed when the FBX uses non-identity mesh geometry transforms.

### Skeletal skinning fix v3
- Reverted the incorrect world-space skin matrices introduced in v2.
- Skin matrices are now converted from `ufbx` world space back into the imported mesh's geometry-local space using `inverse(mesh.geometry_to_world) * cluster.geometry_to_world`.
- The renderer continues to apply the FBX mesh geometry transform exactly once.
- This prevents the skeletal mesh from disappearing due to double application of the FBX geometry transform.
### Skeletal FBX skinning fix v4

- Corrected the FBX skeletal skinning convention to use ufbx's world-space `cluster->geometry_to_world` matrices directly, matching the official ufbx viewer.
- Prevented `CalculateModelMatrix()` from applying `geometry_to_world` a second time to skeletal meshes.
- Removed the mesh-local inverse conversion introduced by the previous fix, which was mixing two transform spaces.
- When a FBX contains several skinned meshes, the single-mesh importer now selects the largest skinned geometry instead of relying on FBX object ordering, and reports that condition explicitly.

## Point-light shadow quality fix

- Fixed point-light shadow depth to be computed per fragment from the interpolated world position instead of interpolating a per-vertex distance.
- Replaced the fixed oversized cube-shadow sampling offsets with a texel-scaled 9-tap kernel.
- Enabled linear filtering on point-light shadow cube maps to reduce hard texel stepping.
- Removed the large polygon offset from point-light shadow rendering; receiver-side bias remains in the lighting shader.
- Skeletal animation pose updates now invalidate scene shadow maps so animated casters update their shadows.

## Automatic FBX shader selection

### Modifié

- `HRL_CreateMaterialFromFBX()` et `HRL_CreateMaterialFromFBXIndexed()` détectent automatiquement la présence de skin deformers dans le FBX et sélectionnent `HRL_SKINNED_3D_MESH_SHADER` pour les matériaux de modèles squelettiques, au lieu d'utiliser systématiquement le shader statique.
- Les variantes `WithShader` restent disponibles pour forcer explicitement un shader.

## FBX resources and automatic materials

### Ajouté

- `HRL_FBXResources`, handle opaque pour conserver une scène FBX décodée dédiée aux matériaux et textures.
- `HRL_LoadFBXResources()` / `HRL_FreeFBXResources()`.
- Accès aux matériaux avec `HRL_GetFBXMaterialCount()`, `HRL_GetFBXMaterial()` et `HRL_FindFBXMaterial()`.
- Accès aux textures avec `HRL_GetFBXTextureCount()`, `HRL_GetFBXTexture()` et `HRL_FindFBXTexture()`.
- `HRL_GetFBXMaterialTexture()` pour récupérer directement la texture correspondant à un slot de matériau.
- `HRL_CreateTextureFromFBX()` pour uploader une texture embarquée vers HRL.
- `HRL_CreateMaterialFromFBX()` et ses variantes `WithShader` / `Indexed` pour reconstruire automatiquement un matériau HRL depuis un FBX.
- Les slots automatiques correspondent aux maps Albedo, Normal, Specular, Roughness, Metallic et Alpha.

### Import FBX

- Les matériaux PBR ufbx sont utilisés en priorité, avec repli sur les propriétés FBX Lambert/Phong lorsque nécessaire.
- Les textures embarquées sont récupérées depuis `ufbx_texture::content`, les fichiers de texture référencés ou les textures fichier internes d'un wrapper layered/shader.
- Les textures externes restent visibles dans l'API avec leur nom/chemin, mais ne sont pas chargées automatiquement par l'API mémoire tant qu'aucun chemin de fichier de base n'est fourni.
- Les octets retournés par `HRL_FBXTextureInfo::data` restent valides tant que `HRL_FBXResources` n'est pas libéré.
- Les textures GPU créées automatiquement par `HRL_CreateMaterialFromFBX*` sont possédées par le matériau et libérées avec celui-ci.

### Exemple

`examples/main.cpp` montre maintenant :

```cpp
HRL_FBXResources* resources = HRL_LoadFBXResources(data, size);
const HRL_FBXMaterialInfo* material = HRL_GetFBXMaterial(resources, 0);
const HRL_FBXTextureInfo* albedo =
    HRL_GetFBXMaterialTexture(resources, 0, HRL_FBX_MATERIAL_ALBEDO);
HRL_FreeFBXResources(resources);

HRL_id mat = HRL_CreateMaterialFromFBX(data, size);
```

Le chemin skeletal existant et les corrections de shadows ne sont pas modifiés par cette fonctionnalité.

### Notes on the automatic FBX material reconstruction
- `HRL_FBXMaterialInfo` keeps the scalar PBR/legacy values (base color, roughness, metallic, specular and opacity) available even when no texture is embedded.
- The built-in automatic material path binds the six texture channels already supported by HRL's built-in 3D shader; custom shaders can consume the exposed scalar material parameters as needed.
- The same embedded GPU texture is reused when one FBX texture is connected to more than one supported material slot.

### Test de la fonctionnalité
`examples/main.cpp` affiche maintenant les matériaux FBX, leurs propriétés de base, les textures associées à chaque slot et les informations d'embedding avant d'appeler `HRL_CreateMaterialFromFBX()`.

## Global illumination API / OpenGL 3.3 SSGI

### Ajouté

- Nouvelle API publique `HRL_SetGlobalIlluminationEnabled()` / `HRL_SetGlobalIlluminationMethod()`.
- `HRL_IsGlobalIlluminationMethodSupported()` et `HRL_GetGlobalIlluminationSupportedMethods()` permettent d'interroger le backend actif.
- Ajout de `HRL_EGlobalIlluminationMethod` avec SSGI, VCT, LPV, DDGI, path tracing et ray tracing.
- Le backend OpenGL 3.3 contient maintenant `backend/opengl33/gi.h` et `gi.cpp` avec une implémentation SSGI par ray marching écran/espace-profondeur, sans compute shader.
- Le GI est strictement opt-in et le rendu est inchangé par défaut.

### Matrice de support actuelle

| Backend | SSGI | VCT | LPV | DDGI | Path tracing | Ray tracing |
|---|---:|---:|---:|---:|---:|---:|
| OpenGL 3.3 | Oui | Non | Non | Non | Non | Non |
| OpenGL 4.5 | Non implémenté | Non implémenté | Non implémenté | Non implémenté | Non implémenté | Non implémenté |
| Vulkan | Non implémenté | Non implémenté | Non implémenté | Non implémenté | Non implémenté | Non implémenté |
| D3D11 | Non implémenté | Non implémenté | Non implémenté | Non implémenté | Non implémenté | Non implémenté |
| D3D12 | Non implémenté | Non implémenté | Non implémenté | Non implémenté | Non implémenté | Non implémenté |
| Metal | Non implémenté | Non implémenté | Non implémenté | Non implémenté | Non implémenté | Non implémenté |
| NVN | Non implémenté | Non implémenté | Non implémenté | Non implémenté | Non implémenté | Non implémenté |
| GNM | Non implémenté | Non implémenté | Non implémenté | Non implémenté | Non implémenté | Non implémenté |

### Notes SSGI

- La première méthode est volontairement une approximation screen-space, destinée au diffuse indirect en un rebond.
- Elle reconstruit la position et la normale depuis le depth buffer, marche des rayons dans l'hémisphère visible et accumule la radiance du scene color buffer.
- Les surfaces ou sources hors écran ne peuvent pas contribuer à cette passe ; il s'agit d'une limitation intrinsèque du screen-space GI.
- Aucun compute shader n'est requis, ce qui permet de conserver cette première méthode sur OpenGL 3.3.

## SSGI v14 — correction du pipeline horizon-based

### Corrigé

- Correction du calcul de `stepRadius` dans le shader SSGI : la distance d'échantillonnage est maintenant divisée par `STEP_COUNT + 1`, conformément au calcul utilisé par l'implémentation SSGI de référence de three.js/SSRT3. L'ancienne version plaçait la majorité des échantillons trop loin du pixel et hors du viewport, ce qui pouvait produire un GI pratiquement nul.
- Le masquage d'horizon utilise maintenant un bitfield 32 bits par slice et le recouvrement déjà échantillonné est soustrait avant d'ajouter une nouvelle contribution.
- Le traitement front/back de l'intervalle d'horizon suit la convention directionnelle de la référence : inversion `yx` selon le côté de la slice.
- Le terme GI brut est maintenant séparé de l'albedo du récepteur ; le composite applique le `GI * albedo` au framebuffer de scène, ce qui correspond au découpage beauté/diffuse utilisé par l'exemple SSGI de three.js.
- La rotation/jitter temporels restent actifs pour fournir des échantillons différents entre les frames, puis la passe temporelle et le denoise existants stabilisent le résultat.

### Test

- Vérification statique : le calcul de rayon utilise `radius * viewportWidth / 32 / (STEP_COUNT + 1)`.
- Vérification statique : le shader contient le bitfield 32 bits et la fonction de population correspondante, et le composite consomme bien l'albedo G-buffer.
- Le skinning, les matériaux FBX et le système d'ombres ne sont pas modifiés dans cette révision.


## GI v15 – Correction critique du SSGI (normales world-space → view-space)
- **Corrigé** le calcul SSGI qui utilisait directement les normales world-space du G-buffer avec des directions/positions view-space.
- **Cause** : les équations horizon-based (SSILVB/SSGI) utilisent `viewPosition`, `viewDir`, `pixelToSample` et `viewNormal` dans le même espace caméra. Mélanger la normale world-space HRL avec ces vecteurs pouvait annuler presque toutes les contributions (`N·L <= 0`) et rendre le composite visuellement identique au GI désactivé.
- `SampleNormalVS()` transforme maintenant explicitement `normalWorld` par `mat3(uView)`.
- Aucun changement au skinning, matériaux FBX ou shadows.

## GI v16 – Correction build du shader SSGI
- **Corrigé** l'oubli du `uniform mat4 uView` dans le fragment shader SSGI après le passage des normales world-space en view-space.
- Sans ce uniform, la référence `mat3(uView)` rendait le shader non compilable, donc `RenderSSGI()` sortait avant toute passe GI.
- Le pipeline conserve le calcul SSGI horizon-based et le G-buffer albedo/normal de v13-v15.

## DDGI / irradiance probe rewrite

- Replaced the brute-force per-frame triangle scan with a cached CPU BVH.
- Geometry is rebuilt only when mesh topology/transforms change.
- Probe updates reduced to a bounded progressive batch per frame.
- Fixed 3D probe texture packing to use OpenGL's X-fastest layout.
- Reworked probe lobes to store directional outgoing radiance rather than mixing irradiance and radiance units.
- Removed the accidental second albedo / 1-pi attenuation in the final composite.
- Directional/spot light directions now use the same pitch/yaw convention as the renderer.
- Restored all five scene framebuffer draw buffers after the GI composite.
- Added probe radiance diagnostics every 30 frames.

## DDGI v8 – forward-shaded world-space GI

- Removed the fullscreen/depth-copy DDGI composite path.
- DDGI is now evaluated directly in the built-in static/skinned 3D mesh fragment shader.
- Probe data is uploaded through a std140 UBO instead of extra texture units.
- Probe updates are one-shot for static lighting/geometry and stop once the volume converges.
- Reduced probe update budget to 8 probes x 12 rays and removed secondary CPU shadow rays from probe capture.
- Added explicit GI shader binding and per-scene probe UBO state.

## Responsive widgets example
- Added `example/main.widgets.cpp` showing framebuffer resize handling with `HRL_WindowResizeCallback`.
- Demonstrates responsive widget anchors for top-left, top-right, bottom-left, bottom-right and center layouts.
- Mouse coordinates are converted from GLFW window coordinates to framebuffer coordinates for HiDPI displays.


### UI text SDF
- Widget labels and button captions now use signed distance field (SDF) text internally.
- Kept the public text/widget API unchanged.
- Added UTF-8 decoding, kerning-aware layout, and derivative-based SDF smoothing.
- The OpenGL UI shader distinguishes SDF text from normal textured widgets.

2026-09-26 - Gizmo modes are now combinable bit flags. Translation, rotation and scale can be displayed together. Scale uses translation-like axis handles, while rotation uses quarter-wheel arcs. Added operation getters and configurable rotation arc size.

- Gizmo rotation arcs now use quarter-wheels aligned to the positive neighboring axes: X rotates from +Y to +Z, Y from +Z to +X, and Z from +X to +Y. Render and picking use the same basis and radius.

- Fixed C++ combined HRL gizmo mode calls by adding a flag-friendly overload for HRL_SetGizmoMode.

## VFX / Particle systems

Added a Niagara-like VFX layer with HRL IDs for systems, emitters and curves. Systems support automatic or manual simulation, looping, time scale and transforms. Emitters support spawn rate/bursts, lifetimes, shapes, velocity, gravity, drag, force, noise, curves, collisions, billboards, stretched billboards, mesh rendering and blend modes. Individual particles are internal simulation data rather than HRL objects. The OpenGL 3.3 backend renders billboard particles with GPU instancing.

- VFX depth handling is now fully internal: transparent particles automatically depth-test against opaque geometry and never write depth.


## VFX mesh particles

- `HRL_VFX_RENDER_MESH` now renders a static HRL 3D mesh as a particle template using the mesh's regular 3D material.
- Added `HRL_SetVFXEmitterMeshScale()` for per-emitter base model scale.
- Added `HRL_SetVFXEmitterMeshRotation()` for per-emitter model orientation offset.
- `HRL_SetVFXEmitterMesh()` validates that the supplied asset is a static `HRL_3D_MESH`.
- Particle size, rotation, curves, simulation space, blending and VFX color still apply to mesh particles.

## Default post-process shader parameters

The built-in OpenGL post-process shader keeps all existing parameters and now also supports:

- `exposure` (float, default 0; stops)
- `hueShift` (float, degrees)
- `sharpenStrength` (float, default 0)
- `chromaticAberration` (float, pixels, default 0)
- `filmGrainStrength` / `filmGrainScale` (float)
- `vignetteStrength`, `vignetteRadius`, `vignetteSoftness` (float)
- `vignetteColor` (vec3)
- `fadeAmount` (float) and `fadeColor` (vec3)

All are set through the existing `HRL_MaterialSet*` API on the post-process material. `uTime` is supplied internally by the OpenGL backend for animated default-shader effects.

- Implemented Screen Space Displacement Mapping for built-in static and skinned 3D materials.

## SS Displacement Mapping pipeline fix
- SS displacement meshes are now rendered normally into the scene first, so the screen-space pass has the actual mesh color as its source image.
- The displacement pass is executed back into the active HRL scene framebuffer after the first MSAA resolve.
- `ResolveSceneMSAA()` now leaves `scene->fbo` bound instead of the default framebuffer.
- A second MSAA resolve is only performed when the displacement pass actually rendered, preventing debug primitives from being overwritten.
- Existing public material API is unchanged.
