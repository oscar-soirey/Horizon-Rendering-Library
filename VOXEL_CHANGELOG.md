# HRL — ajout du monde voxel 2D

## Résumé

Ajout d'un système de monde voxel 2D intégré à HRL. Les voxels sont des carrés dans le plan XY, avec `type == 0` réservé au vide. Le stockage complet reste côté CPU ; le backend OpenGL 3.3 ne crée des buffers GPU que pour les chunks visibles par les caméras de la scène, avec une marge interne d'un chunk.

## Ajouté

- `HRL_Voxel` dans `hrl.h`.
- API C :
  - `HRL_SetVoxelSize`
  - `HRL_SetVoxelPhysicalSize`
  - `HRL_SetVoxelChunkSize`
  - `HRL_SetVoxelTypeColor`
  - `HRL_LoadVoxelWorld`
  - `HRL_SetVoxelType`
  - `HRL_GetVoxelType`
- Stockage interne `HRL_VoxelWorld`.
- Gestion interne des chunks GPU OpenGL 3.3.
- Génération de géométrie par greedy meshing.
- Shader OpenGL 3.3 dédié aux carrés voxel avec couleur RGBA, brouillard et sorties G-buffer existantes.
- `examples/main.cpp` : monde 96×64, chunks 16×16, trois types/couleurs et modification du voxel `(48,48)` avec ESPACE.

## Supprimé

Aucun élément existant de l'API publique ou des backends n'a été supprimé.

## Modifié

### `hrl.h`

Ajout de la structure publique minimale `HRL_Voxel` et des fonctions de gestion du monde voxel. La version d'API passe de `0.6` à `0.7`.

### `core/object_types.h`

Ajout du stockage interne CPU du monde voxel, de la table de couleurs par type et du suivi des chunks à régénérer. Aucun détail de chunk ou de buffer OpenGL n'est exposé dans l'API publique.

### `hrl.cpp`

Ajout de l'implémentation de l'API voxel, de la validation des dimensions/positions et de l'invalidation automatique du chunk concerné lors d'un `HRL_SetVoxelType`.

### `backend/opengl33/gl33_renderer.cpp`

Ajout du streaming des chunks visibles, de la génération greedy des rectangles, de l'upload VAO/VBO/EBO, de la destruction des chunks sortant de la zone utile et du rendu des voxels comme géométrie OpenGL classique.

## Fonctionnement du stockage

Après :

```cpp
HRL_SetVoxelSize(scene, 1024, 1024);
```

l'array transmis à `HRL_LoadVoxelWorld` utilise l'indexation :

```cpp
voxels[y * 1024 + x]
```

`HRL_LoadVoxelWorld` copie le monde complet en RAM mais ne l'upload pas entièrement au GPU. Le backend calcule l'intersection entre le frustum de chaque caméra et le plan XY du monde, détermine les chunks correspondants, ajoute une marge d'un chunk et ne garde que ces géométries OpenGL.

## Greedy meshing

Le meshing est effectué à l'intérieur de chaque chunk. Les voxels voisins de même `type` sont fusionnés en rectangles ; un chunk rempli uniformément produit donc un seul rectangle au lieu d'un carré par voxel. Les chunks restent indépendants afin que leur streaming puisse être effectué sans recréer le monde entier.

## Mise à jour dynamique

```cpp
HRL_SetVoxelType(scene, x, y, type);
```

modifie immédiatement la donnée CPU et marque son chunk comme `dirty`. Au rendu suivant, le chunk est régénéré et son buffer OpenGL est remplacé automatiquement s'il est actuellement chargé.

Changer une couleur avec `HRL_SetVoxelTypeColor` force la régénération des chunks actuellement chargés, puisque la couleur est précuite dans leurs sommets.

## Tests effectués

- Vérification syntaxique de `hrl.h` comme header C11 : OK.
- Vérification syntaxique de `hrl.h` comme header C++17 : OK.
- Test isolé de l'algorithme de greedy meshing avec le code effectivement ajouté :
  - carré plein 4×4 : 1 rectangle / 6 indices ;
  - monde vide : 0 rectangle / 0 indice ;
  - damier 4×4 : 16 rectangles / 96 indices.
- Relecture statique du chemin `LoadVoxelWorld → SyncVoxelChunks → RebuildVoxelChunk → DrawVoxelWorld` et du nettoyage des buffers à la suppression de scène/shutdown.

## Limitation de validation

La compilation complète du renderer n'a pas pu être exécutée depuis cette archive seule : le ZIP fourni ne contient notamment pas `hrl_gl.h` ni `ressources/ressources.h`, alors que le code existant les inclut déjà. Je n'ai donc pas prétendu avoir effectué un build complet de HRL à partir de cette archive.
