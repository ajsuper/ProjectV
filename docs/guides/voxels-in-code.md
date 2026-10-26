# Build voxels in code

Filling components procedurally. Concepts: [The scene](../concepts/scene.md), [Voxel data](../concepts/voxel-data.md),
[Editing](../concepts/editing.md).

| You are making | Do this | Example |
|---|---|---|
| A prop, a body, a shape | `addComponent` (16-voxel chunk) + `queueVoxelAdd` + `updateScene` | [`17-physics`](../../examples/17-physics/) `voxels()` |
| A floor, a wall, an arena, anything big and continuous | **The same.** It grows into one grid of 16-voxel cells | [`16-sandbox`](../../examples/16-sandbox/) `shape()` |
| Many copies of one thing | Build it once, then `duplicateComponent` (geometry shared) | `17-physics` `prototype()` + `place()` |
| Terrain streamed around the camera, built on worker threads | Brick map per chunk, install into a grid yourself | [`50-terrain-generator`](../../examples/50-terrain-generator/) |

## The normal route: a component and the edit queue

```cpp
using namespace projv;

ComponentHandle box = utils::addComponent(scene, ComponentKind::Chunk, "crate",
                                          INVALID_COMPONENT_HANDLE,   // parent: none
                                          16,                         // resolution: a power of four
                                          0.25f);                     // metres per voxel
utils::setComponentTransform(scene, box, {0, 2, 0}, core::quat(1, 0, 0, 0), 1.0f);

std::vector<PendingVoxelOp> ops;
for (int z = 0; z < 8; z++) for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
    ops.push_back({true, {x, y, z}, packRGB10(0.6f, 0.4f, 0.2f)});

utils::queueVoxelAdd(scene, box, ops);
utils::updateScene(scene);
graphics::flushSceneUpdates(scene, gpuData);
```

- **Positions are component voxel coordinates**, `(0,0,0)` at the component's corner. They may run
  past the resolution or go negative.
- **Colours are interned** per op. To choose slots, fill the palette first (`internMaterial` /
  `addMaterial`) and set `op.slot`.
- **`resolution` and `voxelScale` are fixed** for the component's life. A resolution that isn't a
  power of four, or a non-positive scale, gets `INVALID_COMPONENT_HANDLE`.

## Big and continuous: let it become a grid

**Start every component as a 16-voxel chunk, whatever its size.** Ops past the edge turn it into a
grid of 16³ cells (`convertChunkToGrid`, then `expandGridToInclude` as it grows), all under one
component:

```text
 one 256³ chunk for a 128×2×128 floor          one component, grown into a grid of 16³ cells
 ┌──────────────────────────┐                  ┌──┬──┬──┬──┬──┬──┬──┬──┐
 │                          │  a ray entering  │▓▓│▓▓│▓▓│▓▓│▓▓│▓▓│▓▓│▓▓│  only cells that exist,
 │      (empty cube)        │  the cube pays   └──┴──┴──┴──┴──┴──┴──┴──┘  walked by a DDA
 │▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│  a full march
 └──────────────────────────┘
```

The shader marches every loose chunk whose cube a ray enters. The sandbox arena as one 256-voxel
floor chunk plus four wall chunks cost 65 ms a frame on the GPU; as one grid it is a DDA over the
cells it has.

- **One lattice-aligned continuous object is one Grid component**: a floor and the walls on it, on
  one voxel lattice, are one component.
- **Never build a big loose chunk**, and never tile loose chunks yourself.
- **Things that move separately are separate components.**

## Many copies: build once, duplicate

```cpp
ComponentHandle proto = utils::addComponent(scene, ComponentKind::Asset, "crate",
                                            INVALID_COMPONENT_HANDLE, 4, 1.0f);   // a node
// ... add a voxel chunk under it, offset by minus half its size so the node is its centre ...

for (int i = 0; i < 100; i++) {
    ComponentHandle copy = utils::duplicateComponent(scene, proto);    // shares the blob
    utils::setComponentTransform(scene, copy, {i * 2.0f, 0, 0}, core::quat(1, 0, 0, 0), 1.0f);
}
```

Copies share one blob (and one physics shape) until edited. Hide or delete the prototype afterwards.

## Hello-voxel's manual route

[`00-hello-voxel`](../../examples/00-hello-voxel/main.cpp) builds the `Scene` by hand:

```cpp
auto map = utils::createVoxelBrickMap(utils::computeBrickDims(64));
utils::brickMapSetVoxel(*map, x, y, z, slot);                      // slots from internMaterial
utils::updateChunkFromBrickMap(chunk, *map);                        // → chunk.geometryData (a tree64)
std::vector<uint8_t> ids;
utils::bakeMaterialsFromBrickMap(chunk.geometryData, ids, *map);    // → material bytes
int32_t blob = internChunkGeometry(scene, chunk, std::move(map));   // → the pool
scene.geometryPool[blob].materialIDs = std::move(ids);
```

The blob holds the tree until something asks for its tiles (the first upload, edit or collision shape),
when `utils::blobTiles` builds the [tile table](../reference/formats/tile-table.md) and the tree is
dropped. Code that builds tiles itself (a worker) installs them with `utils::setBlobTiles`.

It then pushes the `ComponentRecord` and `Chunk` directly, which is only safe in an empty scene.
Otherwise use `addComponent`, `reserveComponentSlot` + `placeComponent` and `allocateChunkSlot`, so
freed rows are reused and the root index stays right
([The scene](../concepts/scene.md#handles-refs-and-slot-reuse)).

## Streamed and threaded: the terrain generator

[`50-terrain-generator`](../../examples/50-terrain-generator/) streams a heightfield into one
`SceneGrid` from worker threads.

```text
 worker threads                                  main thread, each frame
 ┌──────────────────────────────┐   ready queue   ┌───────────────────────────────────────────┐
 │ createVoxelBrickMap           │ ─────────────▶  │ expandGridToInclude(grid, cell, ...)       │
 │ brickMapSetVoxel ...          │                 │ internChunkGeometry + setBlobTiles         │
 │ createChunk(header)           │                 │ chunk row → grid.cellToChunk[cell]         │
 │ updateChunkFromBrickMap       │                 │ requestChunkLOD(scene, h, wanted)          │
 │ bakeMaterialsFromBrickMap     │                 │ refine: replaceChunkTiles(scene, h, ...)   │
 │ buildTileTable                │                 │ flushSceneUpdates(scene, gpu)              │
 │ internMaterial (thread-safe)  │                 │                                            │
 └──────────────────────────────┘                 └───────────────────────────────────────────┘
```

| Step | API | Note |
|---|---|---|
| Generate off-thread | brick map → `updateChunkFromBrickMap` → `bakeMaterialsFromBrickMap` → `buildTileTable` | Touches no shared state except `internMaterial`, which locks `scene.materialPaletteMutex` |
| Admit on the main thread | `internChunkGeometry` + `setBlobTiles`, set `gridIndex` / `cellIndex` / `componentHandle`, write `cellToChunk` | Mark `headerDirty` on a reused row. The example keeps its own free list; new code should use `allocateChunkSlot`. |
| Grow the grid | `utils::expandGridToInclude(grid, cell, scene, gridIndex)` | `cell` is the **absolute** cell coordinate, not an index rebased on the grid |
| Coarser far away | `requestChunkLOD(scene, h, resolution)` | `Coarsened`: done at upload. `NeedsRegeneration`: generate finer and install it. |
| Finer close up | `replaceChunkTiles(scene, h, tiles, resolution)` (`replaceChunkGeometry` for a tree) | Keeps the handle and cell: no pop while the new data is built |
| Evict | `releaseBlob`, clear `cellToChunk`, kill the chunk | The GPU range is freed on the next flush |

- **255 usable palette slots are shared by every chunk of the component.** Intern from a fixed
  material list, not per-voxel colours.
- **Storage LOD is a VRAM lever, not a frame-time one**
  ([Level of detail](../concepts/voxel-data.md#level-of-detail)).
