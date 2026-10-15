# Load, show and save a scene

A compose folder from disk to the screen and back. Headers:
[`utils/compose_io.h`](../../include/utils/compose_io.h),
[`utils/scene_query.h`](../../include/utils/scene_query.h),
[`graphics/gpu_interface.h`](../../include/graphics/gpu_interface.h). Format:
[compose.json](../reference/formats/compose-json.md).

```text
 folder/compose.json + *.data ──loadComposeFromDisk──▶ Scene ──createTexturesForScene──▶ GPUData
                                                         │
                                 saveComposeToDisk ◀─────┘  (any subtree, to any folder)
```

## Load

```cpp
#include "core/paths.h"
#include "utils/compose_io.h"

const auto here = projv::core::executableDirectory();
auto& scene = app.world.ctx().emplace<projv::Scene>(
    projv::utils::loadComposeFromDisk((here / "scenes/SmallVox").string()));
```

- **Give it the folder**, not the `compose.json`.
- **Repeated `.data` blocks are loaded once** and shared: one pool blob, many chunks.
- **A `.data` older than version 2 is rejected** and its component holds no geometry; watch for
  `Loaded 0 chunk(s)`. See [known latent issues](../plans/known-latent-issues.md).

## Show

```cpp
auto& gpu = app.world.ctx().emplace<projv::GPUData>(projv::graphics::createTexturesForScene(scene));
projv::graphics::flushSceneUpdates(scene, gpu);   // after the Scene changes
```

Window, renderer and draw call: [Your first app](../getting-started/first-app.md).

## Inspect

```cpp
for (const auto& info : projv::utils::listComponents(scene))      // depth-first
    projv::core::info("{}  {}  {} voxels", info.fullPath, info.sourcePath, info.voxelCount);

projv::ComponentHandle door = projv::utils::findComponentByPath(scene, "House/Door");
projv::core::vec3 where    = projv::utils::getComponentWorldPosition(scene, door);
```

| Want | Call |
|---|---|
| Everything, with paths and world positions | `listComponents(scene)` → `ComponentInfo { handle, name, fullPath, kind, sourcePath, worldPosition, voxelCount }` |
| The top level | `rootComponents(scene)` |
| By path / by name / by saved id | `findComponentByPath`, `findComponentsByName`, `findComponentByLocalId` |
| One component's path or voxel count | `getComponentPath`, `getComponentVoxelCount` |
| The folder it came from | `scene.documentPath` |

## Graft another folder in

```cpp
projv::ComponentHandle house = projv::utils::instantiateComposeInto(
    scene, "assets/House", projv::INVALID_COMPONENT_HANDLE,      // parent: none, a root
    {10, 0, 4}, projv::core::quat(1, 0, 0, 0), 1.0f);
projv::graphics::flushSceneUpdates(scene, gpu);
```

The folder becomes one new `Asset` node named after it, with its geometry **copied**, not shared
with what is already loaded. `runtime::instantiatePrefab` also caches the folder, shares geometry
between instances and spawns its `entities.json` ([Entities](../concepts/entities.md#prefabs)).

## Save

```cpp
if (!projv::utils::saveComposeToDisk(scene, projv::INVALID_COMPONENT_HANDLE, "out/MyScene"))
    projv::core::error("save incomplete");   // a compose.json may name a .data that didn't finish
```

| `root` | Writes |
|---|---|
| `INVALID_COMPONENT_HANDLE` | The whole scene: its roots become the folder's `compose.json` |
| An `Asset` handle | That subtree: its children become the folder's `compose.json` |

- One `<name>.data` per chunk or grid component; chunks sharing a blob share one file.
- One subfolder per `Asset` child, written recursively, except **linked** assets
  (`externalSource`), which stay references to their own folder.
- Colliding file names are overwritten; other files are left alone.
- Entities are saved separately: `runtime::saveEntities(world, root, folder)`.

Lower level, for tools: `parseComposeJson` / `writeComposeJson` (the document only),
`readDataFile` / `writeDataFile` (one `.data`), `dataFileFromComponent` (a component's voxels as a
`DataFile`), `readDataFileHeader` + `readDataBlock` (stream single blocks).

## Load a different scene

Free the old GPU mirror first:

```cpp
projv::graphics::destroyGPUData(gpu);
scene = projv::utils::loadComposeFromDisk(otherFolder);
gpu   = projv::graphics::createTexturesForScene(scene);
```

With the runtime, also destroy the old scene's entities and call `runtime::clearPrefabCache(world)`:
its pins name blobs in the old pool.

## Getting your own models in

[`examples/20-mesh-voxelizer`](../../examples/20-mesh-voxelizer/) turns any mesh Assimp reads (OBJ,
FBX, glTF, ...) or a Minecraft world into a compose folder:

```bash
cmake --build --preset dev --target mesh_voxelizer
cd build/examples/mesh_voxelizer
./mesh_voxelizer -f model.glb -o out/Model -r 256      # resolution along the longest axis
```

Check it in [`examples/10-scene-previewer`](../../examples/10-scene-previewer/). It applies no
lighting, so what looks wrong there is wrong in the data:

```bash
cd build/examples/scene_previewer && ./scene_previewer path/to/out/Model
```
