# HRL — Automatic LOD

## Résumé

Ajout d'un système de Level of Detail automatique pour les meshes 3D, entièrement intégré au pipeline OpenGL 3.3 existant.

Le LOD 0 correspond toujours exactement à la géométrie fournie à `HRL_CreateMesh3D()`. Les niveaux supplémentaires sont générés en interne par HRL et uploadés comme des buffers GPU indépendants.

Aucun format de fichier supplémentaire n'est ajouté.

## Ajouté

### API publique

Ajout de l'énumération :

```cpp
typedef enum HRL_ELODMode {
    HRL_LOD_DISTANCE = 0,
    HRL_LOD_SCREEN_SIZE
} HRL_ELODMode;
```

Ajout des fonctions :

```cpp
void HRL_SetMeshLODAutomatic(HRL_id mesh, int enabled);
void HRL_SetMeshLODMode(HRL_id mesh, HRL_ELODMode mode);
void HRL_SetMeshLODLevels(HRL_id mesh, HRL_uint levels);
void HRL_SetMeshLODDistance(HRL_id mesh, float baseDistance);
void HRL_SetMeshLODScale(HRL_id mesh, float distanceScale);
void HRL_SetMeshLODMinDistance(HRL_id mesh, float distance);
void HRL_SetMeshLODMaxDistance(HRL_id mesh, float distance);
void HRL_SetMeshLODScreenThreshold(HRL_id mesh, float threshold);
void HRL_SetMeshLODScreenScale(HRL_id mesh, float scale);
void HRL_SetMeshLODHysteresis(HRL_id mesh, float hysteresis);
void HRL_SetMeshLODOverride(HRL_id mesh, int level);
void HRL_ForceMeshLODRebuild(HRL_id mesh);
HRL_uint HRL_GetMeshLODCount(HRL_id mesh);
size_t HRL_GetMeshLODVertexCount(HRL_id mesh, HRL_uint level);
size_t HRL_GetMeshLODTriangleCount(HRL_id mesh, HRL_uint level);
int HRL_GetMeshLODLevel(HRL_id mesh);
```

### Génération automatique

Les niveaux sont générés par regroupement spatial des vertices (vertex clustering), en conservant les attributs principaux :

- position ;
- normale ;
- UV ;
- tangente ;
- bitangente.

Les dimensions de la grille sont calculées en fonction des proportions du mesh. Les géométries planes ou très fines ne perdent donc pas inutilement la majorité de leurs cellules dans un axe sans volume.

Les transitions utilisent par défaut la progression suivante :

```text
LOD 0 → 100 %
LOD 1 → 50 %
LOD 2 → 25 %
LOD 3 → 12,5 %
LOD 4 → 6,25 %
...
```

Le nombre réellement atteint peut être inférieur à celui demandé si une simplification supplémentaire ne produit pas de géométrie valide.

### Deux modes de sélection

`HRL_LOD_SCREEN_SIZE` : mode automatique par taille apparente à l'écran. Il tient compte de la taille du bounding sphere et de la projection de la caméra.

`HRL_LOD_DISTANCE` : mode par distance au mesh avec seuil initial et facteur multiplicatif.

Le mode par taille écran est le mode par défaut lorsqu'on active le LOD automatique.

### Hystérésis

`HRL_SetMeshLODHysteresis()` empêche les changements rapides de niveau lorsque la caméra reste autour d'une frontière de transition.

Valeur par défaut : `0.05`.

### Override

```cpp
HRL_SetMeshLODOverride(mesh, 2);
```

force le LOD 2.

```cpp
HRL_SetMeshLODOverride(mesh, -1);
```

revient au choix automatique.

## Intégration GPU

Le backend OpenGL conserve maintenant une ressource GPU par niveau :

```text
MeshGPU
 ├── LOD 0
 ├── LOD 1
 ├── LOD 2
 ├── ...
 └── LOD N
```

Le frustum culling est effectué avant la sélection du niveau, puis le niveau choisi est utilisé par le batching/draw call.

Les mêmes niveaux peuvent être utilisés pendant la génération des shadow maps.

## Optimisations associées

Le LOD est intégré après le frustum culling afin de ne pas effectuer de sélection de niveau pour les objets déjà hors frustum.

Le tri de rendu conserve le regroupement par shader, matériau et VAO ; deux meshes utilisant des LOD différents ne sont donc pas regroupés artificiellement.

La géométrie LOD est générée uniquement lors de la configuration/reconstruction et non à chaque frame.

## Compatibilité API

Aucune fonction publique existante n'a été supprimée.

Les nouvelles fonctions sont additives.

`hrl_gl.h` reste inchangé.

Les sprites ne sont pas soumis au LOD automatique : ils conservent leur chemin de rendu 2D/plane existant.

## Exemple

Le projet contient un seul exemple :

```text
examples/
├── main.cpp
└── sky_equirectangular.png
```

`main.cpp` conserve la structure du template existant et active le LOD automatique du mesh FBX :

```cpp
HRL_SetMeshLODLevels(model, 5);
HRL_SetMeshLODMode(model, HRL_LOD_SCREEN_SIZE);
HRL_SetMeshLODScreenThreshold(model, 0.22f);
HRL_SetMeshLODScreenScale(model, 0.5f);
HRL_SetMeshLODHysteresis(model, 0.08f);
HRL_SetMeshLODAutomatic(model, HRL_TRUE);
```

L'exemple affiche également le nombre de niveaux générés et le nombre de triangles/vertices de chaque niveau.

La touche `5` active la vue de debug LOD.

## Vérifications effectuées

- compilation syntaxique de `hrl.h` en C11 ;
- compilation syntaxique de `hrl.h` en C++20 ;
- vérification que toutes les fonctions LOD ajoutées sont présentes à la fois dans l'en-tête et l'implémentation ;
- vérification qu'aucun `_filePath` ne reste dans l'API/source concernés ;
- vérification qu'il n'y a qu'un seul fichier source d'exemple : `examples/main.cpp` ;
- test de l'algorithme de simplification sur une grille de 14 641 vertices / 28 800 triangles :
  - LOD 1 : 7 396 vertices / 14 450 triangles ;
  - LOD 2 : 3 600 vertices / 6 962 triangles ;
  - LOD 3 : 1 849 vertices / 3 528 triangles ;
  - LOD 4 : 900 vertices / 1 682 triangles.

Le build final OpenGL/MinGW n'est pas exécuté dans cet environnement Linux ; les dépendances natives du projet restent nécessaires pour ce dernier contrôle.

## Correctif de compilation — RebuildLODs

### Corrigé
- Correction de la déclaration forward interne de `RebuildLODs()` dans `hrl.cpp`.
- La déclaration utilise maintenant la même liaison interne `static` que la définition.

### Impact API
- Aucun changement de l'API publique.
- `hrl.h` et `hrl_gl.h` inchangés.

### Test
- Le fichier contient maintenant une déclaration `static bool RebuildLODs(HRL_Mesh* mesh);` et une définition `static bool RebuildLODs(HRL_Mesh* mesh)` cohérentes.

## Correctif compilation — LOD renderer

### Corrigé
- `hrl.cpp` : la déclaration forward de `RebuildLODs(HRL_Mesh*)` est maintenant dans le même namespace anonyme que sa définition, supprimant la déclaration externe/orpheline.
- `backend/opengl33/gl33_renderer.cpp` : ajout de la définition de `GetMeshLOD_GPU()`.
- `backend/opengl33/gl33_renderer.cpp` : ajout des prototypes de `DrawOpaqueMeshes()` et `DrawSprites()` avant leur utilisation dans `GL33_DrawScene()`.
- `backend/opengl33/gl33_renderer.cpp` : adaptation de `GL33_Shutdown()` au nouveau `MeshGPU::levels` pour libérer les VAO/VBO/EBO de chaque niveau LOD.
- `backend/opengl33/gl33_renderer.cpp` : suppression de la seconde définition de `GL33_CreateSpriteMesh()`.

### Vérifications
- Test C++ isolé de la liaison d'une déclaration dans namespace anonyme vers sa définition : OK.
- Une seule définition de `GL33_CreateSpriteMesh()` : OK.
- `GetMeshLOD_GPU()` défini une fois : OK.
- `RebuildLODs()` déclaré et défini dans le même namespace : OK.
- Aucun champ obsolète `MeshGPU::vao/vbo/ebo` utilisé directement : OK.
- Aucun fichier `fake_*`, `append_test` ou fichier de travail ajouté à la livraison.
